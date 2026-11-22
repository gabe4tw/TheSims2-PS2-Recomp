#include "quickmenu.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace quickmenu {
namespace {

// ---------------------------------------------------------------------------------------------------------------------------------
// The pages: what is on them, in reading order (the left column top to bottom, then the right column). On the settings page a
// control with a heading starts a category (a framed group); the categories are what the pad moves over first.

struct Control {
    Command cmd;
    Kind kind;
    const char *label;
    int column;               // 0 left, 1 right
    const char *heading;      // starts a category with this name, or nullptr
    bool disabled;            // shown greyed ("coming later"), never focused
    const char *key;          // the program's own key for it, shown dim (main page)
    const char *help;         // the description shown while it is focused
    const char *heading_help; // the category's description (with a heading)
};

constexpr Control kMain[] = {
    {Command::resume, Kind::button, "Resume", 0, nullptr, false, "F1", "Back to the game.", nullptr},
    {Command::save, Kind::button, "Quick save", 0, nullptr, false, "F5", "Saves the whole game as it is now into the quick save slot.", nullptr},
    {Command::load, Kind::button, "Quick load", 0, nullptr, false, "F8", "Goes back to the last quick save.", nullptr},
    {Command::open_states, Kind::button, "Save states", 0, nullptr, false, "", "Ten save slots: save the game as it is now into a slot, or load one.", nullptr},
    {Command::open_cards, Kind::button, "Memory cards", 0, nullptr, false, "",
     "Your memory cards: put a card into slot 1 or 2, make new ones, and name, copy or delete them. Each card holds one story and one freeplay save.", nullptr},
    {Command::open_settings, Kind::button, "Settings", 0, nullptr, false, "", "Picture, speed and controller settings.", nullptr},
    {Command::open_cheats, Kind::button, "Cheats", 0, nullptr, false, "",
     "The game's own cheats: the gnome cheats you would enter with button codes, and hidden ones (free items, god mode...).", nullptr},
    {Command::open_debug, Kind::button, "Debug", 0, nullptr, false, "", "Every debug setting the developers left in the game, with their own notes.", nullptr},
    {Command::restart, Kind::button, "Restart the game", 0, nullptr, false, "",
     "Starts the game again, so settings marked \"needs restart\" are used. Progress since your last save is lost.", nullptr},
    {Command::quit, Kind::button, "Quit the game", 0, nullptr, false, "", "Writes the memory card and closes the game.", nullptr},
};
// local change: the launcher's main page (Mode::launcher), in the order the user chose
constexpr Control kLauncherMain[] = {
    {Command::play, Kind::button, "Play", 0, nullptr, false, "", "Starts the game. The launcher comes back when you quit it.", nullptr},
    {Command::open_cards, Kind::button, "Memory cards", 0, nullptr, false, "",
     "Your memory cards: choose the cards in slot 1 and 2 for the next game, make new ones, and name, copy or delete them.", nullptr},
    {Command::open_mods, Kind::button, "Mods", 0, nullptr, false, "", "Mods: other people's texture packs, settings and patches. Install them from a zip, turn them on or off, choose which wins.", nullptr},
    {Command::open_settings, Kind::button, "Settings", 0, nullptr, false, "", "Picture, speed and controller settings. They are used when the game starts.", nullptr},
    {Command::quit, Kind::button, "Quit", 0, nullptr, false, "", "Closes the launcher.", nullptr},
};
// local change: the same with the HD textures to download first (Menu::set_hd)
constexpr Control kLauncherMainHd[] = {
    {Command::get_hd, Kind::button, "Get the HD textures", 0, nullptr, false, "",
     "Downloads the HD textures (SomberTwilight's texture pack, used with permission) from their official link. They are part of the game: "
     "Settings > Textures switches between them and the original textures.", nullptr},
    kLauncherMain[0], kLauncherMain[1], kLauncherMain[2], kLauncherMain[3], kLauncherMain[4],
};
// the same with a newer release to install first (Menu::set_update)
constexpr Control kLauncherMainUpdate[] = {
    {Command::update_now, Kind::button, "Update", 0, nullptr, false, "", "Downloads the newer release and builds it. Your cards, settings and mods stay.", nullptr},
    kLauncherMain[0], kLauncherMain[1], kLauncherMain[2], kLauncherMain[3], kLauncherMain[4],
};
constexpr Control kGraphics[] = {
    {Command::look_filter, Kind::slider, "Filter", 0, "Picture", false, "",
     "How the picture is fitted to your screen: Off shows the plain pixels, Crisp keeps hard pixels with soft borders, Soft blurs gently, Sharp sharpens.",
     "How the game's picture is drawn: filter, resolution, colours and textures."},
    {Command::opt_aa, Kind::slider, "Anti-aliasing", 0, nullptr, false, "",
     "Smooths jagged edges. Fast: a light filter on the finished picture (costs almost nothing). 2x to 8x: draws the game that many times larger "
     "each way over Render resolution and shrinks it - smoothest, but 2x is already about four times the graphics work. At most 8x the PS2's size "
     "in all (Render resolution 3x: 2x is the most). Used at the next start.",
     nullptr},
    {Command::scale, Kind::slider, "Render resolution", 0, nullptr, false, "",
     "Draws the game at 1 to 4 times the PS2's size. Higher is sharper but needs more from the graphics card.", nullptr},
    {Command::opt_deband, Kind::radio, "Smooth colour bands", 0, nullptr, false, "",
     "Hides the stepped colour bands of the game's 16-bit colours, for example in skies and on walls.", nullptr},
    {Command::opt_textures, Kind::radio, "Textures", 0, nullptr, false, "",
     "Updated uses the HD texture pack wherever it has a picture; Original keeps the game's own textures.", nullptr},
    {Command::fullscreen, Kind::radio, "Window", 1, "Display", false, "",
     "A window, a borderless window over the whole screen, or true full screen.", "How the game sits on your screen."},
    {Command::opt_screen_res, Kind::slider, "Screen resolution", 1, nullptr, false, "",
     "The size of the window, or the screen mode in true full screen (a borderless window always covers the desktop).", nullptr},
    {Command::opt_aspect, Kind::radio, "Aspect ratio", 1, nullptr, false, "",
     "The shape of the picture: 4:3 like an old TV, 16:9, or 21:9 for ultrawide screens. The game draws its view for that shape.", nullptr},
    {Command::opt_vsync, Kind::checkbox, "V-sync", 1, nullptr, false, "",
     "Shows each picture in step with your screen's refresh: no tearing and even motion, a little more input delay. Off shows pictures as soon as they are ready.",
     nullptr},
    {Command::opt_ui_width, Kind::slider, "Menus and HUD width", 1, "Interface", false, "",
     "75 % keeps menus and the HUD in their true shape on a wide screen; 100 % stretches them like the original. (4:3: always the original.)",
     "How menus, text and the HUD look on a wide screen."},
    {Command::sharp_2d, Kind::radio, "Text and menus", 1, nullptr, false, "", "How sharp the game's text, icons and menus are when enlarged.", nullptr},
};
constexpr Control kGame[] = {
    {Command::opt_frame_rate, Kind::list, "Frame rate", 0, "Speed", false, "",
     "50 fps is the original. Higher is smoother; a rate above your screen's refresh rate is held to the screen's.",
     "How many pictures per second the game draws."},
    {Command::opt_load_turbo, Kind::checkbox, "Fast-forward loading", 1, "Loading", false, "",
     "Runs the game at full speed while it loads, so loading screens end sooner.", "Shorter waits while the game reads the disc."},
    {Command::opt_disc_speed, Kind::slider, "Disc drive speed", 1, nullptr, false, "",
     "How fast the disc is read. 1x is the original; faster gives shorter loads.", nullptr},
    {Command::opt_skip_intro, Kind::checkbox, "Skip intro", 1, nullptr, false, "",
     "Leaves out the EA logo and the intro movie when the game starts (the Maxis logo stays).", nullptr},
};
constexpr Control kControls[] = {
    {Command::opt_player1, Kind::list, "Player 1", 0, "Players", false, "",
     "The controller that plays player 1 (press any button on a controller to see which one it is). \"All controllers\": every controller plays player 1. \"Keyboard & mouse\": the keyboard and the mouse play this player (no controller); chosen for nobody, they play player 1 together with its controller.",
     "Which controller plays player 1 and player 2. Every controller can always work this menu."},
    {Command::opt_player2, Kind::list, "Player 2", 0, nullptr, false, "", "The controller that plays player 2, or nobody (press any button on a controller to see which one it is).", nullptr},
    {Command::keys, Kind::button, "Show keys", 1, "Keys and buttons", false, "", "Shows a card with every key and button.",
     "See and change the keyboard keys and controller buttons."},
    {Command::change_keys, Kind::button, "Change keys", 1, nullptr, false, "", "Which keyboard keys and mouse buttons work which PS2 button.", nullptr},
    {Command::change_pad, Kind::button, "Change controller buttons", 1, nullptr, false, "", "Which controller button works which PS2 button.", nullptr},
    {Command::opt_mouse_mode, Kind::radio, "Mouse", 1, "Mouse", false, "",
     "PC style: in the cursor view (Select) the cursor goes where you point, left click picks, a short right click goes back, hold the right button and move to turn the camera, the wheel zooms, the screen edges scroll. As a controller: the mouse buttons work the PS2 buttons set under Change keys.",
     "How the mouse plays the game."},
    {Command::opt_webcam, Kind::list, "EyeToy camera", 0, "EyeToy", false, "",
     "The webcam the game uses as its EyeToy USB camera (pause menu > EyeToy: photos for your Sims' picture slots). None leaves the camera unplugged.",
     "Use a webcam of this PC as the EyeToy camera."},
};
constexpr const char *kTabNames[3] = {"Graphics", "Game", "Controls"};

// the buttons below the categories
struct Button {
    Command cmd;
    const char *label;
    const char *help;
};
constexpr Button kButtons[kActionCount] = {
    {Command::preset_vanilla, "Vanilla",
     "Fills in the original PS2 picture and disc speed: 1x resolution, original textures, stretched HUD, 1x disc. Press Apply to use it."},
    {Command::preset_updated, "Updated",
     "Fills in the remaster's picture and disc speed: 3x resolution, HD textures, Fast anti-aliasing, true-shape HUD, smooth colour bands, 4x disc. Press Apply to use it."},
    {Command::defaults, "Defaults", "Puts this tab back to its standard settings. Press Apply to use them."},
    {Command::apply, "Apply changes", "Uses every choice marked \"not applied\". Some are used only after Restart (main menu)."},
};

struct Span {
    const Control *at;
    int n;
    const Control &operator[](int i) const { return at[i]; }
};
template <int N> constexpr Span span(const Control (&a)[N]) { return {a, N}; }

Span main_controls(Mode mode, bool update = false, bool hd = false) {
    return mode == Mode::launcher ? (update ? span(kLauncherMainUpdate) : hd ? span(kLauncherMainHd) : span(kLauncherMain)) : span(kMain);
}

// the Controls tab's buttons that need the game window (its key capture): the launcher shows them greyed
bool needs_game_window(Command c) { return c == Command::keys || c == Command::change_keys || c == Command::change_pad; }

Span page_controls(Page page, int tab) {
    if (page == Page::main) return span(kMain);
    return tab == 0 ? span(kGraphics) : tab == 1 ? span(kGame) : span(kControls);
}

struct Group {
    int first = 0, count = 0, column = 0;
    const char *title = "", *help = "";
};

std::vector<Group> groups_of(int tab) {
    const Span page = page_controls(Page::settings, tab);
    std::vector<Group> out;
    for (int i = 0; i < page.n; ++i) {
        if (page[i].heading || out.empty()) out.push_back({i, 0, page[i].column, page[i].heading ? page[i].heading : "", page[i].heading_help ? page[i].heading_help : ""});
        ++out.back().count;
    }
    return out;
}

int group_of(int tab, int control) {
    const std::vector<Group> gs = groups_of(tab);
    for (int g = 0; g < static_cast<int>(gs.size()); ++g)
        if (control >= gs[g].first && control < gs[g].first + gs[g].count) return g;
    return 0;
}

const Setting &setting_of(const Values &v, Command c) { return v.settings[static_cast<int>(c)]; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// Moving around

Menu::Menu(Mode mode) : mode_(mode) { clear_changes(); }

void Menu::show(Page page) {
    page_ = page;
    key_sel_ = key_top_ = 0;
    focus_ = 0;
    dialog_ = {};
    status_.clear();
}

int Menu::main_count() const { return main_controls(mode_, !update_.empty(), !hd_label_.empty()).n; }
bool Menu::main_disabled(int i) const { return main_controls(mode_, !update_.empty(), !hd_label_.empty())[i].disabled; }

void Menu::open() {
    open_ = true;
    page_ = Page::main;
    tab_ = 0;
    focus_ = 0;  // always starts on Resume, so a second press of the key that opened it gives the game back
    main_return_ = 4;
    dialog_ = {};
    editing_ = false;
    slot_col_ = 0;
    inside_ = false;
    cell_ = row_ = hl_ = 0;
    list_open_ = false;
    armed_ = Command::none;
    discard_armed_ = false;
    capturing_ = false;
    defaults_armed_ = false;
    key_sel_ = key_top_ = 0;
    status_.clear();
    clear_changes();
}

int Menu::category_count() const { return static_cast<int>(groups_of(tab_).size()); }

int Menu::change_count() const {
    int n = 0;
    for (int p : pending_) n += p >= 0;
    return n;
}

std::vector<Action> Menu::changes() const {
    std::vector<Action> out;
    for (int c = 0; c < static_cast<int>(Command::count_); ++c)
        if (pending_[c] >= 0) out.push_back({static_cast<Command>(c), pending_[c]});
    return out;
}

void Menu::clear_changes() { std::fill(std::begin(pending_), std::end(pending_), -1); }

int Menu::shown(Command c, const Values &v) const {
    const int p = pending_[static_cast<int>(c)];
    return p >= 0 ? p : setting_of(v, c).index;
}

void Menu::pick(Command c, int choice, const Values &v) {
    const Setting &s = setting_of(v, c);
    if (s.locked || choice < 0 || choice >= static_cast<int>(s.choices.size())) return;
    pending_[static_cast<int>(c)] = choice == s.index ? -1 : choice;  // the value in use again: nothing to apply
}

Action Menu::press(Input in, const Values &v) {
    if (!open_) return {};
    status_.clear();
    if (dialog_.open) return press_dialog(in);
    if (editing_) return press_keyboard(in);
    if (page_ == Page::states) return press_states(in, v);
    if (page_ == Page::cards) return press_cards(in, v);
    if (page_ == Page::mods) return press_mods(in, v);
    if (page_ == Page::install) return press_install(in, v);
    if (page_ == Page::pick_iso) return press_picker(in, v);
    if (page_ == Page::progress) return press_progress(in, v);
    if (page_ == Page::keys || page_ == Page::pad) return press_keys(in);
    if (list_page()) return press_list_page(in, v);
    if (list_open_) return press_list(in, v);
    if (page_ == Page::main) return press_main(in);
    return press_settings(in, v);
}

Action Menu::press_keys(Input in) {
    const int rows = key_rows_ + 1;  // the buttons, then "Put back the defaults"
    switch (in) {
    case Input::up:
    case Input::down:
        key_sel_ = (key_sel_ + (in == Input::up ? -1 : 1) + rows) % rows;
        defaults_armed_ = false;
        fit_keys();
        return {};
    case Input::confirm:
        if (key_sel_ < key_rows_) {
            capturing_ = true;
            return {Command::key_edit};
        }
        ask(page_ == Page::pad ? "Put back the default controller buttons?" : "Put back the default keys?", {Command::key_defaults}, false);
        return {};
    case Input::back:
        page_ = Page::settings;
        tab_ = 2;
        inside_ = true;
        row_ = keys_return_;
        cell_ = group_of(tab_, row_);
        capturing_ = defaults_armed_ = false;
        return {};
    default: return {};
    }
}

Action Menu::press_list(Input in, const Values &v) {
    const Control &c = page_controls(page_, tab_)[row_];
    const int n = static_cast<int>(setting_of(v, c.cmd).choices.size());
    switch (in) {
    case Input::up: list_hl_ = std::max(0, list_hl_ - 1); return {};
    case Input::down: list_hl_ = std::min(n - 1, list_hl_ + 1); return {};
    case Input::confirm:
        list_open_ = false;
        if (groups_of(tab_)[cell_].count == 1) inside_ = false;  // (a category with only this list: back to the categories)
        if (setting_of(v, c.cmd).locked) return {c.cmd, list_hl_};  // (the window only says why it cannot change)
        pick(c.cmd, list_hl_, v);
        return {};
    case Input::back:
        list_open_ = false;
        if (groups_of(tab_)[cell_].count == 1) inside_ = false;
        return {};
    case Input::tab_prev:
    case Input::tab_next:
        list_open_ = false;
        switch_tab(in == Input::tab_prev ? -1 : 1);
        return {};
    default: return {};
    }
}

Action Menu::press_main(Input in) {
    const Span page = main_controls(mode_, !update_.empty(), !hd_label_.empty());
    switch (in) {
    case Input::up:
    case Input::down: {
        const int by = in == Input::up ? -1 : 1;
        do focus_ = (focus_ + by + page.n) % page.n;
        while (page[focus_].disabled);
        armed_ = Command::none;
        return {};
    }
    case Input::confirm: {
        const Command c = page[focus_].cmd;
        if (c == Command::quit) {
            ask(mode_ == Mode::launcher ? "Close the launcher?" : "Quit the game? The memory card is written first; anything not saved in the game is lost.", {c},
                false);
            return {};
        }
        if (c == Command::restart) {
            ask("Restart the game? Anything not saved in the game is lost.", {c}, false);
            return {};
        }
        if (c == Command::get_hd) {  // local change: the HD textures (asks first: what and how big)
            if (hd_busy_) ask("Stop the download of the HD textures? What is downloaded so far is thrown away.", {Command::hd_cancel}, false);
            else
                ask("Download the HD textures? About 98 MB from their official Google Drive link (SomberTwilight's texture pack, used with "
                    "permission). They are checked and put into the game folder.", {Command::get_hd}, true);
            return {};
        }
        if (c == Command::open_states) {
            main_return_ = focus_;
            page_ = Page::states;
            key_sel_ = 0;
        }
        if (c == Command::open_cards || c == Command::open_mods) {
            main_return_ = focus_;
            page_ = c == Command::open_cards ? Page::cards : Page::mods;
            key_sel_ = key_top_ = 0;
            card_col_ = 0;
        }
        armed_ = Command::none;
        if (c == Command::open_settings) {
            main_return_ = focus_;
            page_ = Page::settings;
            tab_ = 0;
            inside_ = false;
            cell_ = 0;
            discard_armed_ = false;
        }
        if (c == Command::open_cheats || c == Command::open_debug) {
            main_return_ = focus_;
            page_ = c == Command::open_cheats ? Page::cheats : Page::debug;
            key_sel_ = key_top_ = 0;
            defaults_armed_ = false;
            key_rows_ = 0;  // (the window fills the rows in; the first focusable one is found on the first press or drawing)
        }
        return {c};
    }
    case Input::back:
        armed_ = Command::none;
        if (mode_ == Mode::launcher) {  // there is no game to go back to: Back asks to close the launcher
            ask("Close the launcher?", {Command::quit}, false);
            return {};
        }
        return {Command::resume};
    default: armed_ = Command::none; return {};
    }
}

void Menu::switch_tab(int by) {
    tab_ = (tab_ + by + 3) % 3;
    inside_ = false;
    cell_ = 0;
    discard_armed_ = false;
}

Action Menu::press_settings(Input in, const Values &v) {
    if (in == Input::tab_prev || in == Input::tab_next) {
        switch_tab(in == Input::tab_prev ? -1 : 1);
        return {};
    }
    if (in != Input::back) discard_armed_ = false;
    return inside_ ? press_inside(in, v) : press_grid(in, v);
}

// local change: Vanilla and Updated (buttons 0 and 1) are shown on the Graphics tab only (they fill every tab wherever they are
// pressed, so one place is enough); the other tabs show Defaults and Apply.
int Menu::first_button() const { return tab_ == 0 ? 0 : 2; }

// the first level: the categories in two columns, the buttons in a row below them
Action Menu::press_grid(Input in, const Values &v) {
    const std::vector<Group> gs = groups_of(tab_);
    const int G = static_cast<int>(gs.size());
    std::vector<int> col[2];
    for (int g = 0; g < G; ++g) col[gs[g].column].push_back(g);
    const auto place_in = [&](int g, int &c, int &p) {
        c = gs[g].column;
        p = static_cast<int>(std::find(col[c].begin(), col[c].end(), g) - col[c].begin());
    };
    switch (in) {
    case Input::up:
        if (cell_ >= G) {
            const int first = first_button();
            int c = (cell_ - G - first) * 2 < kActionCount - first ? 0 : 1;  // the left half of the shown buttons: the left column
            if (col[c].empty()) c = 1 - c;
            if (!col[c].empty()) cell_ = col[c].back();
        } else {
            int c, p;
            place_in(cell_, c, p);
            if (p > 0) cell_ = col[c][p - 1];
        }
        return {};
    case Input::down:
        if (cell_ < G) {
            int c, p;
            place_in(cell_, c, p);
            cell_ = p + 1 < static_cast<int>(col[c].size()) ? col[c][p + 1] : G + (c == 0 ? first_button() : first_button() == 0 ? 2 : 3);
        }
        return {};
    case Input::left:
    case Input::right: {
        const int by = in == Input::left ? -1 : 1;
        if (cell_ >= G) {
            cell_ = G + std::clamp(cell_ - G + by, first_button(), kActionCount - 1);
            return {};
        }
        int c, p;
        place_in(cell_, c, p);
        const int other = c + by;
        if (other >= 0 && other <= 1 && !col[other].empty()) cell_ = col[other][std::min(p, static_cast<int>(col[other].size()) - 1)];
        return {};
    }
    case Input::confirm:
        if (cell_ >= G) return do_button(cell_ - G, v);
        enter(gs[cell_].first, v);
        // a category with one setting: straight into it (a list opens at once; leaving it goes back to the categories)
        if (gs[cell_].count == 1 && page_controls(page_, tab_)[row_].kind == Kind::list) return use(row_, Input::confirm, v);
        return {};
    case Input::back:
        if (change_count() > 0) {
            const int n = change_count();
            ask("Throw away " + std::to_string(n) + (n == 1 ? " change" : " changes") + " that " + (n == 1 ? "is" : "are") + " not applied?",
                {Command::discard_changes}, false);
            return {};
        }
        clear_changes();
        page_ = Page::main;
        focus_ = main_return_;
        return {};
    default: return {};
    }
}

void Menu::enter(int control, const Values &v) {
    const Span page = page_controls(page_, tab_);
    inside_ = true;
    row_ = std::clamp(control, 0, page.n - 1);
    cell_ = group_of(tab_, row_);
    const Setting &s = setting_of(v, page[row_].cmd);
    hl_ = std::clamp(shown(page[row_].cmd, v), 0, std::max(0, static_cast<int>(s.choices.size()) - 1));
}

// inside a category: up / down from setting to setting, the rest goes to the setting
Action Menu::press_inside(Input in, const Values &v) {
    const Group g = groups_of(tab_)[cell_];
    switch (in) {
    case Input::up:
    case Input::down:
        enter(std::clamp(row_ + (in == Input::up ? -1 : 1), g.first, g.first + g.count - 1), v);
        return {};
    case Input::back: inside_ = false; return {};
    default: return use(row_, in, v);
    }
}

// left, right or confirm on a control of the settings page: left / right moves the highlight, confirm picks it (not used before Apply)
Action Menu::use(int control, Input in, const Values &v) {
    const Span page = page_controls(page_, tab_);
    const Control &c = page[control];
    const Setting &s = setting_of(v, c.cmd);
    const int n = static_cast<int>(s.choices.size());
    const bool side = in == Input::left || in == Input::right;
    switch (c.kind) {
    case Kind::radio:
    case Kind::slider:
        if (side) {
            hl_ = std::clamp(hl_ + (in == Input::left ? -1 : 1), 0, std::max(0, n - 1));
            if (c.kind == Kind::radio) return {};
            // a slider takes the step at once (still waiting for Apply), as before; only round buttons are highlighted first
            if (s.locked) return {c.cmd, hl_};
            pick(c.cmd, hl_, v);
            return {};
        }
        if (in != Input::confirm || n == 0) return {};
        if (s.locked) return {c.cmd, hl_};
        pick(c.cmd, hl_, v);
        return {};
    case Kind::list:
        if (in != Input::confirm || n == 0) return {};
        list_open_ = true;
        list_hl_ = std::clamp(shown(c.cmd, v), 0, n - 1);
        return {};
    case Kind::checkbox:
        if (in != Input::confirm) return {};
        if (s.locked) return {c.cmd, shown(c.cmd, v) ? 0 : 1};
        pick(c.cmd, shown(c.cmd, v) ? 0 : 1, v);
        return {};
    case Kind::button:
        if (in != Input::confirm) return {};
        if (mode_ == Mode::launcher && needs_game_window(c.cmd)) {
            status_ = "Keys and controller buttons are changed in the game's quick menu (F1, or L3 + R3 on a controller).";
            return {};
        }
        if (c.cmd == Command::change_keys || c.cmd == Command::change_pad) {
            keys_return_ = control;
            page_ = c.cmd == Command::change_pad ? Page::pad : Page::keys;
            key_sel_ = key_top_ = 0;
            capturing_ = defaults_armed_ = false;
        }
        return {c.cmd};
    }
    return {};
}

// the buttons below the categories
Action Menu::do_button(int button, const Values &v) {
    const Command cmd = kButtons[std::clamp(button, 0, kActionCount - 1)].cmd;
    if (cmd == Command::apply) {
        const int n = change_count();
        if (n) ask("Apply " + std::to_string(n) + (n == 1 ? " change?" : " changes?"), {Command::apply}, true);
        return {};
    }
    for (int t = 0; t < 3; ++t) {
        if (cmd == Command::defaults && t != tab_) continue;  // Defaults: this tab only; the presets: every tab
        const Span page = page_controls(Page::settings, t);
        for (int i = 0; i < page.n; ++i) {
            const Setting &s = setting_of(v, page[i].cmd);
            const int to = cmd == Command::preset_vanilla ? s.vanilla : cmd == Command::preset_updated ? s.updated : s.fallback;
            if (to >= 0) pick(page[i].cmd, to, v);
        }
    }
    cell_ = category_count() + 3;  // on to Apply
    return {cmd};
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The cheats and debug pages: one list of rows (headings, checkboxes, numbers) and a last row that puts back the game's own values

namespace {
bool focusable(const Values &v, int row) { return row >= static_cast<int>(v.list_rows.size()) || !v.list_rows[row].header; }
}  // namespace

void Menu::fit_list(const Values &v) {
    key_rows_ = static_cast<int>(v.list_rows.size());
    const int rows = key_rows_ + 1;
    key_sel_ = std::clamp(key_sel_, 0, rows - 1);
    while (key_sel_ < rows - 1 && !focusable(v, key_sel_)) ++key_sel_;
    fit_keys();
    if (key_sel_ > 0 && key_sel_ < key_rows_ && key_top_ == key_sel_ && !focusable(v, key_sel_ - 1)) key_top_ = key_sel_ - 1;  // show its heading
}

Action Menu::press_list_page(Input in, const Values &v) {
    fit_list(v);
    const int rows = key_rows_ + 1;  // the rows, then "Put back the game's own values"
    switch (in) {
    case Input::up:
    case Input::down: {
        const int by = in == Input::up ? -1 : 1;
        int to = key_sel_;
        do to = (to + by + rows) % rows;
        while (!focusable(v, to) && to != key_sel_);
        key_sel_ = to;
        defaults_armed_ = false;
        fit_list(v);
        return {};
    }
    case Input::left:
    case Input::right:
        defaults_armed_ = false;
        if (key_sel_ < key_rows_ && v.list_rows[key_sel_].number) return {Command::list_step, key_sel_, in == Input::left ? -1 : 1};
        return {};
    case Input::confirm:
        if (key_sel_ < key_rows_) {
            defaults_armed_ = false;
            const ListRow &r = v.list_rows[key_sel_];
            if (r.checkbox || r.action) return {Command::list_toggle, key_sel_};
            if (r.number) return {Command::list_step, key_sel_, 1};
            return {};
        }
        ask(page_ == Page::cheats ? "Turn every cheat on this page off?" : "Put every debug setting back to the game's own value?",
            {Command::list_reset_all}, false);
        return {};
    case Input::back:
        page_ = Page::main;
        focus_ = main_return_;
        defaults_armed_ = false;
        return {};
    case Input::tab_prev:
    case Input::tab_next: {  // a page further up or down
        const int shown = std::max(1, keys_shown_);
        int to = std::clamp(key_sel_ + (in == Input::tab_prev ? -shown : shown), 0, rows - 1);
        while (to < rows - 1 && !focusable(v, to)) ++to;
        key_sel_ = to;
        fit_list(v);
        return {};
    }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The question (Yes / No) and the save states page

void Menu::ask(const std::string &text, Action yes, bool default_yes) {
    dialog_.open = true;
    dialog_.text = text;
    dialog_.yes = yes;
    dialog_.sel = default_yes ? 0 : 1;
}

Action Menu::press_dialog(Input in) {
    switch (in) {
    case Input::left:
    case Input::right:
    case Input::up:
    case Input::down: dialog_.sel ^= 1; return {};
    case Input::back: dialog_.open = false; return {};
    case Input::confirm: {
        dialog_.open = false;
        if (dialog_.sel != 0) return {};
        const Action yes = dialog_.yes;
        if (yes.cmd == Command::discard_changes) {  // the menu's own
            clear_changes();
            page_ = Page::main;
            focus_ = main_return_;
            return {};
        }
        return yes;
    }
    default: return {};
    }
}

// row 0: Load (left / right pick the slot), rows 1 to 10: the slots (Cross saves into one)
Action Menu::press_states(Input in, const Values &v) {
    const auto used = [&](int slot) { return slot >= 1 && slot <= 10 && v.slots[slot - 1].used; };
    if (!used(load_slot_))
        for (int s = 1; s <= 10; ++s)
            if (used(s)) {
                load_slot_ = s;
                break;
            }
    switch (in) {
    case Input::up:
    case Input::down:
        key_sel_ = (key_sel_ + (in == Input::up ? -1 : 1) + 11) % 11;
        if (key_sel_ == 0 || !used(key_sel_)) slot_col_ = 0;
        return {};
    case Input::left:
    case Input::right: {
        if (key_sel_ != 0) {  // a slot's row: Save or Rename (Rename only for a used slot)
            slot_col_ = in == Input::right && used(key_sel_) ? 1 : 0;
            return {};
        }
        const int by = in == Input::left ? -1 : 1;
        for (int k = 1, s = load_slot_; k <= 10; ++k) {
            s = (s - 1 + by + 10) % 10 + 1;
            if (used(s)) {
                load_slot_ = s;
                break;
            }
        }
        return {};
    }
    case Input::confirm:
        if (key_sel_ == 0) {
            if (used(load_slot_))
                ask("Load slot " + std::to_string(load_slot_) + " (" + v.slots[load_slot_ - 1].when + ")? The game goes back to that moment.",
                    {Command::state_load, load_slot_}, false);
            return {};
        }
        if (slot_col_ == 1 && used(key_sel_)) {  // Rename: the keyboard opens with the slot's name
            editing_ = true;
            edit_slot_ = key_sel_;
            edit_card_ = -1;
            edit_text_ = v.slots[key_sel_ - 1].name;
            kb_row_ = 1;
            kb_col_ = 0;
            kb_caps_ = edit_text_.empty();
            return {};
        }
        if (used(key_sel_)) {
            ask("Save over slot " + std::to_string(key_sel_) + " (" + v.slots[key_sel_ - 1].when + ")?", {Command::state_save, key_sel_}, false);
            return {};
        }
        return {Command::state_save, key_sel_};
    case Input::back:
        page_ = Page::main;
        focus_ = main_return_;
        return {};
    default: return {};
    }
}

// The memory cards page (local change). Rows: 0 / 1 the slots (Cross takes the card out), 2 "New card", then one row per card with
// five buttons (left / right): Slot 1, Slot 2, Rename, Copy, Delete. L1 / R1 jump a page of cards.
Action Menu::press_cards(Input in, const Values &v) {
    const int n = static_cast<int>(v.cards.size()), rows = kCardFirstRow + n;
    if (in == Input::back) {
        page_ = Page::main;
        focus_ = main_return_;
        return {};
    }
    if (!v.card_library) return {};
    key_sel_ = std::clamp(key_sel_, 0, rows - 1);
    const auto fit = [&] {
        const int card = key_sel_ - kCardFirstRow;
        if (card >= 0 && card < key_top_) key_top_ = card;
        if (card >= key_top_ + cards_shown_) key_top_ = card - cards_shown_ + 1;
        key_top_ = std::clamp(key_top_, 0, std::max(0, n - cards_shown_));
    };
    switch (in) {
    case Input::up:
    case Input::down:
        key_sel_ = (key_sel_ + (in == Input::up ? -1 : 1) + rows) % rows;
        fit();
        return {};
    case Input::left:
    case Input::right:
        if (key_sel_ >= kCardFirstRow) card_col_ = std::clamp(card_col_ + (in == Input::left ? -1 : 1), 0, kCardColumns - 1);
        return {};
    case Input::tab_prev:
    case Input::tab_next:
        if (n > 0) {
            const int card = std::max(0, key_sel_ - kCardFirstRow);
            key_sel_ = kCardFirstRow + std::clamp(card + (in == Input::tab_prev ? -cards_shown_ : cards_shown_), 0, n - 1);
            fit();
        }
        return {};
    case Input::confirm: {
        if (key_sel_ < 2) {
            const int slot = key_sel_ + 1;
            const std::string &name = v.slot_cards[key_sel_];
            if (name.empty()) {
                status_ = "Slot " + std::to_string(slot) + " is empty. Choose a card below and press its \"Slot " + std::to_string(slot) + "\" button.";
                return {};
            }
            ask("Take \"" + name + "\" out of slot " + std::to_string(slot) + "? The game sees the slot empty, as if the card was pulled out.",
                {Command::card_eject, slot}, false);
            return {};
        }
        if (key_sel_ == 2) return {Command::card_new};
        const int idx = key_sel_ - kCardFirstRow;
        const Values::Card &c = v.cards[idx];
        switch (card_col_) {
        case 0:
        case 1: {
            const int slot = card_col_ + 1;
            if (c.slot == slot) {
                status_ = "\"" + c.name + "\" is in slot " + std::to_string(slot) + " already.";
                return {};
            }
            if (c.slot != 0) {
                status_ = "\"" + c.name + "\" is in slot " + std::to_string(c.slot) + ". Take it out there first (the slot rows at the top).";
                return {};
            }
            return {Command::card_insert, idx, slot};
        }
        case 2:
            editing_ = true;
            edit_card_ = idx;
            edit_slot_ = 0;
            edit_text_ = c.name;
            kb_row_ = 1;
            kb_col_ = 0;
            kb_caps_ = edit_text_.empty();
            return {};
        case 3: return {Command::card_copy, idx};
        default:
            if (c.slot != 0) {
                status_ = "\"" + c.name + "\" is in slot " + std::to_string(c.slot) + ". Take it out first, then delete it.";
                return {};
            }
            ask("Delete \"" + c.name + "\" for good? Every save on it is lost and cannot be brought back.", {Command::card_delete, idx}, false);
            return {};
        }
    }
    default: return {};
    }
}

// The launcher's Mods page (local change, stage 3)
Action Menu::press_mods(Input in, const Values &v) {
    const int n = static_cast<int>(v.mods.rows.size()), rows = kModFirstRow + n;
    if (in == Input::back) {
        page_ = Page::main;
        focus_ = main_return_;
        return {};
    }
    key_sel_ = std::clamp(key_sel_, 0, rows - 1);
    const auto fit = [&] {
        const int mod = key_sel_ - kModFirstRow;
        if (mod >= 0 && mod < key_top_) key_top_ = mod;
        if (mod >= key_top_ + mods_shown_) key_top_ = mod - mods_shown_ + 1;
        key_top_ = std::clamp(key_top_, 0, std::max(0, n - mods_shown_));
    };
    switch (in) {
    case Input::up:
    case Input::down:
        key_sel_ = (key_sel_ + (in == Input::up ? -1 : 1) + rows) % rows;
        fit();
        return {};
    case Input::left:
    case Input::right:
        if (key_sel_ >= kModFirstRow) card_col_ = std::clamp(card_col_ + (in == Input::left ? -1 : 1), 0, kModColumns - 1);
        return {};
    case Input::tab_prev:
    case Input::tab_next:
        if (n > 0) {
            const int mod = std::max(0, key_sel_ - kModFirstRow);
            key_sel_ = kModFirstRow + std::clamp(mod + (in == Input::tab_prev ? -mods_shown_ : mods_shown_), 0, n - 1);
            fit();
        }
        return {};
    case Input::confirm: {
        if (key_sel_ == 0) return {Command::mod_zip};
        const int idx = key_sel_ - kModFirstRow;
        const Values::ModRow &m = v.mods.rows[static_cast<std::size_t>(idx)];
        switch (card_col_) {
        case 0:
            if (!m.on && !m.can_on) {
                status_ = "\"" + m.name + "\" is made for another game and cannot be turned on.";
                return {};
            }
            return {Command::mod_toggle, idx};
        case 1:
            if (idx == 0) {
                status_ = "\"" + m.name + "\" is at the top already.";
                return {};
            }
            key_sel_ -= 1;
            fit();
            return {Command::mod_up, idx};
        case 2:
            if (idx == n - 1) {
                status_ = "\"" + m.name + "\" is at the bottom already.";
                return {};
            }
            key_sel_ += 1;
            fit();
            return {Command::mod_down, idx};
        default:
            ask("Delete the mod \"" + m.name + "\" for good? Its folder is removed from the mods folder.", {Command::mod_delete, idx}, false);
            return {};
        }
    }
    default: return {};
    }
}

// The launcher's install pages (local change, stage 2)
Action Menu::press_install(Input in, const Values &v) {
    switch (in) {
    case Input::up:
    case Input::down: key_sel_ = key_sel_ ? 0 : 1; return {};
    case Input::confirm:
        if (key_sel_ == 0) return {Command::pick_iso};
        if (!v.install.can_install) {
            status_ = !v.install.disc_line.empty() ? v.install.disc_line : "Choose your disc image of The Sims 2 (PAL, SLES-53718) first.";
            return {};
        }
        return {Command::install_start};
    case Input::back: ask("Close the launcher?", {Command::quit}, false); return {};
    default: return {};
    }
}

Action Menu::press_picker(Input in, const Values &v) {
    const int n = static_cast<int>(v.picker.entries.size());
    constexpr int kShown = 12;
    const auto fit = [&] {
        if (key_sel_ < key_top_) key_top_ = key_sel_;
        if (key_sel_ >= key_top_ + kShown) key_top_ = key_sel_ - kShown + 1;
        key_top_ = std::clamp(key_top_, 0, std::max(0, n - kShown));
    };
    switch (in) {
    case Input::up:
    case Input::down:
        if (n > 0) key_sel_ = (key_sel_ + (in == Input::up ? -1 : 1) + n) % n;
        fit();
        return {};
    case Input::tab_prev:
    case Input::tab_next:
        if (n > 0) key_sel_ = std::clamp(key_sel_ + (in == Input::tab_prev ? -kShown : kShown), 0, n - 1);
        fit();
        return {};
    case Input::confirm:
        if (key_sel_ < 0 || key_sel_ >= n) return {};
        return {Command::picker_open, key_sel_};
    case Input::back: return {Command::picker_open, -1};
    default: return {};
    }
}

Action Menu::press_progress(Input in, const Values &v) {
    if (in != Input::confirm && in != Input::back) return {};
    if (!v.progress.error.empty()) {
        if (in == Input::back) {
            ask("Close the launcher? The install goes on from where it stopped next time.", {Command::quit}, false);
            return {};
        }
        return {Command::install_retry};
    }
    if (v.progress.done) return in == Input::confirm ? Action{Command::play} : Action{};
    ask("Stop the install? It goes on from where it stopped next time.", {Command::install_cancel}, false);
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The on-screen keyboard for a slot's name: four rows of ten keys, then Caps / Space / Delete / Done (two to four columns wide)

namespace {
constexpr int kKbCols = 10, kKbRows = 5, kNameMax = 28;
constexpr const char *kKbKeys[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,.'"};
// the bottom row: which key a column belongs to (0 Caps, 1 Space, 2 Delete, 3 Done)
int bottom_key(int col) { return col < 2 ? 0 : col < 6 ? 1 : col < 8 ? 2 : 3; }
// a UTF-8 text without its last code point
void drop_last(std::string &t) {
    if (t.empty()) return;
    std::size_t i = t.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) --i;
    t.erase(i);
}
std::size_t code_points(const std::string &t) {
    std::size_t n = 0;
    for (unsigned char c : t) n += (c & 0xC0) != 0x80;
    return n;
}
}  // namespace

void Menu::type_text(const std::string &utf8) {
    for (unsigned char c : utf8)
        if (c < 0x20) return;  // (no control characters)
    const bool was_empty = edit_text_.empty();
    if (code_points(edit_text_) + code_points(utf8) <= kNameMax) edit_text_ += utf8;
    if (was_empty && !edit_text_.empty()) kb_caps_ = false;  // capital first letter, then small (on either keyboard)
}

void Menu::erase_text() {
    drop_last(edit_text_);
    if (edit_text_.empty()) kb_caps_ = true;  // a name starts with a capital again
}

Action Menu::finish_edit(bool keep) {
    editing_ = false;
    const int card = edit_card_;
    edit_card_ = -1;
    if (!keep) return {};
    while (!edit_text_.empty() && edit_text_.back() == ' ') edit_text_.pop_back();
    if (card >= 0) return {Command::card_rename, card};
    return {Command::slot_rename, edit_slot_};
}

Action Menu::press_keyboard(Input in) {
    switch (in) {
    case Input::up: kb_row_ = (kb_row_ + kKbRows - 1) % kKbRows; return {};
    case Input::down: kb_row_ = (kb_row_ + 1) % kKbRows; return {};
    case Input::left:
    case Input::right: {
        const int by = in == Input::left ? -1 : 1;
        if (kb_row_ == 4) {  // jump from wide key to wide key
            const int key = (bottom_key(kb_col_) + by + 4) % 4;
            kb_col_ = key == 0 ? 0 : key == 1 ? 2 : key == 2 ? 6 : 8;
        } else {
            kb_col_ = (kb_col_ + by + kKbCols) % kKbCols;
        }
        return {};
    }
    case Input::tab_prev: erase_text(); return {};          // L1: delete
    case Input::tab_next: kb_caps_ = !kb_caps_; return {};  // R1: capitals
    case Input::back: return finish_edit(false);             // Triangle: cancel
    case Input::confirm:
        if (kb_row_ < 4) {
            char c = kKbKeys[kb_row_][kb_col_];
            if (kb_caps_ && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            type_text(std::string(1, c));
            if (kb_caps_ && c >= 'A' && c <= 'Z' && code_points(edit_text_) == 1) kb_caps_ = false;  // capital first letter, then small
            return {};
        }
        switch (bottom_key(kb_col_)) {
        case 0: kb_caps_ = !kb_caps_; return {};
        case 1: type_text(" "); return {};
        case 2: erase_text(); return {};
        default: return finish_edit(true);
        }
    default: return {};
    }
}

void Menu::fit_keys() {
    const int shown_rows = std::max(1, keys_shown_);
    if (key_sel_ < key_rows_) {
        if (key_sel_ < key_top_) key_top_ = key_sel_;
        if (key_sel_ >= key_top_ + shown_rows) key_top_ = key_sel_ - shown_rows + 1;
    }
    key_top_ = std::clamp(key_top_, 0, std::max(0, key_rows_ - shown_rows));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The mouse

bool Menu::hover(int x, int y, const Values &v) {
    for (const Hit &h : hits_) {
        if (x < h.box.x || y < h.box.y || x >= h.box.x + h.box.w || y >= h.box.y + h.box.h) continue;
        if (dialog_.open) {
            if (h.dialog < 0 || h.dialog == dialog_.sel) return false;
            dialog_.sel = h.dialog;
            return true;
        }
        if ((page_ == Page::cards || page_ == Page::mods) && h.key_row >= 0) {
            const int col = h.choice >= 0 ? h.choice : card_col_;
            if (h.key_row == key_sel_ && col == card_col_) return false;
            key_sel_ = h.key_row;
            card_col_ = col;
            return true;
        }
        if (h.key_row >= 0) {
            if (capturing_ || h.key_row == key_sel_) return false;
            key_sel_ = h.key_row;
            defaults_armed_ = false;
            return true;
        }
        if (list_open_) {
            if (h.choice >= 0 && h.control < 0 && h.choice != list_hl_) {
                list_hl_ = h.choice;
                return true;
            }
            return false;
        }
        if (page_ == Page::main) {
            if (h.control < 0 || h.control >= main_count() || main_disabled(h.control) || h.control == focus_) return false;
            focus_ = h.control;
            armed_ = Command::none;
            return true;
        }
        if (h.control >= 0) {
            const int g = group_of(tab_, h.control);
            if (inside_ && g == cell_) {
                if (h.control == row_) return false;
                enter(h.control, v);
                return true;
            }
            if (!inside_ && g != cell_) {
                cell_ = g;
                return true;
            }
            return false;
        }
        if (h.cell >= 0 && !inside_ && h.cell != cell_) {
            cell_ = h.cell;
            return true;
        }
        return false;
    }
    return false;
}

Action Menu::click(int x, int y, const Values &v) {
    status_.clear();
    for (const Hit &h : hits_) {
        if (x < h.box.x || y < h.box.y || x >= h.box.x + h.box.w || y >= h.box.y + h.box.h) continue;
        if (dialog_.open) {
            if (h.dialog < 0) return {};
            dialog_.sel = h.dialog;
            return press_dialog(Input::confirm);
        }
        if (editing_) {  // a key of the keyboard (dialog = 100 + row * 10 + column)
            if (h.dialog < 100) return {};
            kb_row_ = (h.dialog - 100) / kKbCols;
            kb_col_ = (h.dialog - 100) % kKbCols;
            return press_keyboard(Input::confirm);
        }
        if (h.back) return back_click(v);  // local change
        if (page_ == Page::states && h.key_row >= 0) {
            key_sel_ = h.key_row;
            return press_states(Input::confirm, v);
        }
        if ((page_ == Page::install || page_ == Page::pick_iso || page_ == Page::progress) && h.key_row >= 0) {
            key_sel_ = page_ == Page::progress ? key_sel_ : h.key_row;
            return page_ == Page::install ? press_install(Input::confirm, v) : page_ == Page::pick_iso ? press_picker(Input::confirm, v) : press_progress(Input::confirm, v);
        }
        if (page_ == Page::cards && h.key_row >= 0) {
            key_sel_ = h.key_row;
            if (h.key_row >= kCardFirstRow && h.choice < 0) return {};  // a card's name: only focus (its buttons do things)
            if (h.choice >= 0) card_col_ = h.choice;
            return press_cards(Input::confirm, v);
        }
        if (page_ == Page::mods && h.key_row >= 0) {  // local change (launcher stage 3)
            key_sel_ = h.key_row;
            if (h.key_row >= kModFirstRow && h.choice < 0) return {};  // a mod's name: only focus (its buttons do things)
            if (h.choice >= 0) card_col_ = h.choice;
            return press_mods(Input::confirm, v);
        }
        if (h.tab >= 0) {
            list_open_ = false;
            if (h.tab != tab_) switch_tab(h.tab - tab_);
            return {};
        }
        if (h.key_row >= 0) {
            if (capturing_) return {};
            key_sel_ = h.key_row;
            if (list_page()) return press_list_page(Input::confirm, v);
            return press_keys(Input::confirm);
        }
        if (list_open_) {
            // a row of the open list picks it; anything else closes the list
            if (h.control < 0 && h.choice >= 0) {
                list_hl_ = h.choice;
                return press_list(Input::confirm, v);
            }
            list_open_ = false;
            return {};
        }
        if (page_ == Page::main) {
            if (h.control < 0 || h.control >= main_count() || main_disabled(h.control)) return {};
            focus_ = h.control;
            return press_main(Input::confirm);
        }
        if (h.control >= 0) {
            const Span page = page_controls(page_, tab_);
            if (h.control >= page.n || page[h.control].disabled) return {};
            const Control &c = page[h.control];
            if (!inside_ || row_ != h.control) enter(h.control, v);
            if (h.choice >= 0 && (c.kind == Kind::radio || c.kind == Kind::slider)) hl_ = h.choice;
            return use(row_, Input::confirm, v);
        }
        if (h.cell >= 0) {
            discard_armed_ = false;
            const std::vector<Group> gs = groups_of(tab_);
            const int G = static_cast<int>(gs.size());
            if (h.cell >= G) {
                inside_ = false;
                cell_ = h.cell;
                return do_button(h.cell - G, v);
            }
            if (!inside_ || cell_ != h.cell) enter(gs[h.cell].first, v);
            return {};
        }
        return {};
    }
    return {};
}

Box Menu::hit_box(Command c, int choice) const {
    const Span page = page_controls(page_, tab_);
    for (const Hit &h : hits_)
        if (h.control >= 0 && h.control < page.n && page[h.control].cmd == c && h.choice == choice && h.tab < 0) return h.box;
    return {};
}

// The look (local change): the game's own menus as the model - see-through periwinkle panels with dark navy rounded borders, a title
// tab on top with the green plumbob, icon tabs with L1 / R1 badges, a light title strip, tiles that turn lime green when focused.
// Every size is written for a window 900 pixels high and multiplied by k (window height / 900, at least 0.6, less when the window is
// too small for the panel).

namespace {

using menudraw::hex;
constexpr std::uint32_t kPanelFill = hex(0x8FA0E0, 217), kBorder = hex(0x1E2A78), kInner = hex(0x9DACE8, 235), kStrip = hex(0xC8D2F5, 235),
                        kText = hex(0x1E2A78), kSoftText = hex(0x4A559A), kDisabled = hex(0x8C8FB8), kSelect = hex(0xA8E070),
                        kGreen = hex(0x3CC83C), kLightGreen = hex(0x9BF08A), kDarkGreen = hex(0x2A9E2A), kAmber = hex(0xE8A030),
                        kTile = hex(0xB8C3F2, 235), kTileOff = hex(0xB8C3F2, 120), kTabOff = hex(0x8193D9, 235), kTabOn = hex(0xAAB8EC, 250),
                        kBadge = hex(0x353A50), kWhite = hex(0xF4F6FF), kBox = hex(0xEEF1FF), kScroll = hex(0xC8D2F5),
                        kGroup = hex(0xA6B4EC, 235), kGroupLit = hex(0xC9EDA6, 240), kHelp = hex(0xDDE3FA, 235);

const menufont::Font &menu_font() {
    static menufont::Font font;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::string path = ps2::os::env("PS2_MENU_FONT");  // UTF-8, as main.cpp set it
        if (!path.empty() && !font.load(path)) std::fprintf(stderr, "[menu] the font %s is not usable: the menu uses its 5 x 8 letters\n", path.c_str());
    }
    return font;
}

// k for a panel W x H (written for a window 900 high), so that it fits the window
float panel_scale(int ww, int wh, float W, float H) {
    const float k = std::max(0.6f, static_cast<float>(wh) / 900.0f);
    return std::max(0.05f, std::min({k, 0.95f * static_cast<float>(ww) / W, 0.95f * static_cast<float>(wh) / H}));
}

void place(Picture &out, int ww, int wh, float W, float H, float k) {
    out.width = std::max(1, std::min(ww, static_cast<int>(std::ceil(W * k))));
    out.height = std::max(1, std::min(wh, static_cast<int>(std::ceil(H * k))));
    out.x = std::max(0, (ww - out.width) / 2);
    out.y = std::max(0, (wh - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, 0);
}

// draws in panel units (multiplied by k)
struct Painter {
    menudraw::Canvas c;
    const menufont::Font &f;
    float k;
    float S(float v) const { return v * k; }
    float width(const std::string &t, float px) const { return f.width(t, S(px)) / k; }
    float line(float px) const { return f.line_height(S(px)) / k; }
    void text(float x, float y, float px, const std::string &t, std::uint32_t col, bool bold = false) {
        f.draw(c, S(x), S(y), S(px), t, col);
        if (bold) f.draw(c, S(x) + std::max(0.6f, k * 0.7f), S(y), S(px), t, col);
    }
    // text in the band y..y+h from x, cut to max_w
    void text_in(float x, float y, float h, float max_w, float px, const std::string &t, std::uint32_t col, bool bold = false) {
        text(x, y + (h - line(px)) / 2, px, menufont::fit_text(f, t, S(px), S(max_w)), col, bold);
    }
    void text_centre(float x, float y, float w, float h, float px, const std::string &t, std::uint32_t col, bool bold = false) {
        const std::string s = menufont::fit_text(f, t, S(px), S(w));
        text(x + (w - width(s, px)) / 2, y + (h - line(px)) / 2, px, s, col, bold);
    }
    void text_right(float right, float y, float h, float px, const std::string &t, std::uint32_t col) {
        text(right - width(t, px), y + (h - line(px)) / 2, px, t, col);
    }
    void rrect(float x, float y, float w, float h, float r, std::uint32_t fill, float border = 0, std::uint32_t edge = kBorder) {
        c.fill_round_rect(S(x), S(y), S(w), S(h), S(r), fill);
        if (border > 0) c.stroke_round_rect(S(x), S(y), S(w), S(h), S(r), S(border), edge);
    }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, std::uint32_t col) { c.fill_triangle(S(x0), S(y0), S(x1), S(y1), S(x2), S(y2), col); }
    void stroke(float x0, float y0, float x1, float y1, float w, std::uint32_t col) { c.line(S(x0), S(y0), S(x1), S(y1), S(w), col); }
    void circle(float cx, float cy, float r, std::uint32_t fill, float border = 0) {
        c.fill_circle(S(cx), S(cy), S(r), fill);
        if (border > 0) c.stroke_circle(S(cx), S(cy), S(r), S(border), kBorder);
    }
    // text broken into lines of at most max_w (at spaces), at most `lines` lines (the last one cut with "...")
    void paragraph(float x, float y, float max_w, float px, const std::string &t, std::uint32_t col, int lines) {
        std::vector<std::string> out;
        std::string cur;
        for (std::size_t i = 0; i < t.size();) {
            std::size_t sp = t.find_first_of(" \n", i);  // local change: '\n' starts a new line
            if (sp == std::string::npos) sp = t.size();
            const std::string word = t.substr(i, sp - i);
            const std::string next = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && width(next, px) > max_w) {
                out.push_back(cur);
                cur = word;
            } else {
                cur = next;
            }
            if (sp < t.size() && t[sp] == '\n') {
                out.push_back(cur);
                cur.clear();
            }
            i = sp + 1;
        }
        if (!cur.empty()) out.push_back(cur);
        if (static_cast<int>(out.size()) > lines && lines > 0) {  // the rest goes on the last line, which is cut with "..."
            std::string rest;
            for (std::size_t n = static_cast<std::size_t>(lines) - 1; n < out.size(); ++n) rest += (rest.empty() ? "" : " ") + out[n];
            out.resize(static_cast<std::size_t>(lines) - 1);
            out.push_back(rest);
        }
        const float lh = line(px) * 1.15f;
        for (std::size_t n = 0; n < out.size(); ++n) text(x, y + n * lh, px, menufont::fit_text(f, out[n], S(px), S(max_w)), col);
    }
};

// ---- icons, drawn from shapes in a square of side s at x, y ----

void plumbob(Painter &p, float cx, float y, float s) {
    const float w = s * 0.3f, mid = y + s * 0.42f, bottom = y + s;
    p.tri(cx, y, cx - w, mid, cx + w, mid, kGreen);
    p.tri(cx - w, mid, cx + w, mid, cx, bottom, kDarkGreen);
    p.tri(cx, y, cx - w, mid, cx, mid, kLightGreen);  // the lit facets
    p.tri(cx - w, mid, cx, mid, cx, bottom, kGreen);
}

// The tab icons (the user's pictures, redrawn): outlines in one line width, with points given on a 512 x 512 grid. Lines are drawn
// as round-capped pieces, so joins come out round. `bg` is the tab's own fill (to hollow the D-pad).
struct Icon {
    Painter &p;
    float x, y, s;
    std::uint32_t col;
    float line = 30;  // the line width on the 512 grid
    float X(float v) const { return x + s * v / 512.0f; }
    float Y(float v) const { return y + s * v / 512.0f; }
    float L(float v) const { return s * v / 512.0f; }
    void seg(float ax, float ay, float bx, float by, float w = -1) { p.stroke(X(ax), Y(ay), X(bx), Y(by), L(w < 0 ? line : w), col); }
    // a cubic curve from a to d with the handles b, c
    void curve(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy) {
        constexpr int n = 18;
        float px = ax, py = ay;
        for (int i = 1; i <= n; ++i) {
            const float t = static_cast<float>(i) / n, u = 1 - t;
            const float qx = u * u * u * ax + 3 * u * u * t * bx + 3 * u * t * t * cx + t * t * t * dx;
            const float qy = u * u * u * ay + 3 * u * u * t * by + 3 * u * t * t * cy + t * t * t * dy;
            seg(px, py, qx, qy);
            px = qx;
            py = qy;
        }
    }
    // a smooth closed line through the points (Catmull-Rom)
    void loop(const float (*pt)[2], int n) {
        for (int i = 0; i < n; ++i) {
            const float *a = pt[(i + n - 1) % n], *b = pt[i], *c = pt[(i + 1) % n], *d = pt[(i + 2) % n];
            curve(b[0], b[1], b[0] + (c[0] - a[0]) / 6, b[1] + (c[1] - a[1]) / 6, c[0] - (d[0] - b[0]) / 6, c[1] - (d[1] - b[1]) / 6, c[0], c[1]);
        }
    }
    void outline_box(float x0, float y0, float x1, float y1, float r) {
        p.c.stroke_round_rect(p.S(X(x0)), p.S(Y(y0)), p.S(L(x1 - x0)), p.S(L(y1 - y0)), p.S(L(r)), p.S(L(line)), col);
    }
    void box(float x0, float y0, float x1, float y1, float r, std::uint32_t fill) { p.rrect(X(x0), Y(y0), L(x1 - x0), L(y1 - y0), L(r), fill); }
    void dot(float cx, float cy, float r) { p.circle(X(cx), Y(cy), L(r), col); }
};

void icon_monitor(Painter &p, float x, float y, float s, std::uint32_t col) {
    Icon i{p, x, y, s, col, 36};
    i.outline_box(20, 56, 492, 376, 52);
    i.box(238, 376, 274, 420, 0, col);   // the stand
    i.box(132, 420, 380, 456, 4, col);   // the foot
}

void icon_hourglass(Painter &p, float x, float y, float s, std::uint32_t col) {
    Icon i{p, x, y, s, col, 30};
    i.line = 26;
    i.outline_box(90, 8, 422, 70, 16);     // the caps (hollow, as in the picture)
    i.outline_box(90, 442, 422, 504, 16);
    i.line = 30;
    i.curve(128, 70, 128, 170, 236, 210, 236, 256);  // the glass, left then right, top half then bottom half
    i.curve(236, 256, 236, 302, 128, 342, 128, 442);
    i.curve(384, 70, 384, 170, 276, 210, 276, 256);
    i.curve(276, 256, 276, 302, 384, 342, 384, 442);
    i.curve(150, 168, 210, 140, 300, 196, 362, 164);  // the sand's top, a wave
    i.curve(170, 442, 196, 392, 316, 392, 342, 442);  // the heap below
    i.seg(256, 306, 256, 318);                        // falling grains
    i.seg(256, 352, 256, 364);
}

void icon_controller(Painter &p, float x, float y, float s, std::uint32_t col, std::uint32_t bg) {
    Icon i{p, x, y, s, col, 30};
    static const float body[][2] = {{110, 96},  {190, 86},  {256, 112}, {322, 86},  {402, 96},  {468, 170}, {496, 300}, {470, 386},
                                    {408, 392}, {362, 330}, {256, 306}, {150, 330}, {104, 392}, {42, 386},  {16, 300},  {44, 170}};
    i.loop(body, 16);
    // the cross pad: a filled plus, hollowed with the tab's own colour
    i.box(66, 184, 214, 250, 26, col);
    i.box(107, 143, 173, 291, 26, col);
    i.box(92, 205, 188, 229, 10, bg);
    i.box(128, 169, 152, 265, 10, bg);
    // the four buttons
    i.dot(370, 172, 22);
    i.dot(326, 216, 22);
    i.dot(414, 216, 22);
    i.dot(370, 260, 22);
}

float title_tab_width(const Painter &p, const std::string &title) { return std::max(220.0f, p.width(title, 28) + 80); }

// the tab with the page's name on top of the panel, the plumbob beside it; drawn before the body
void title_tab(Painter &p, float W, float body_top, const std::string &title) {
    const float tw = title_tab_width(p, title), tx = (W - tw) / 2;
    p.rrect(tx, 2, tw, body_top + 12, 20, kTabOn, 3);
    plumbob(p, tx - 30, 2, 44);
    p.text_centre(tx, 4, tw, body_top - 4, 28, title, kText, true);
}

// after the body: the tab's lower edge disappears into the body
void join_title_tab(Painter &p, float W, float body_top, const std::string &title) {
    const float tw = title_tab_width(p, title), tx = (W - tw) / 2;
    p.c.fill_rect(p.S(tx + 3), p.S(body_top - 2), p.S(tw - 6), p.S(8), kTabOn);
}

void body(Painter &p, float W, float H, float top) { p.rrect(1.5f, top, W - 3, H - top - 1.5f, 22, kPanelFill, 3); }

// a small tag ("needs restart", "not applied") at x, centred on the band y..y+h; returns its width
float tag_width(const Painter &p, const std::string &t) { return p.width(t, 15) + 14; }
float tag(Painter &p, float x, float y, float h, const std::string &t, std::uint32_t fill) {
    const float w = tag_width(p, t);
    p.rrect(x, y + (h - 22) / 2, w, 22, 11, fill, 1, kBorder);
    p.text_centre(x, y + (h - 22) / 2, w, 22, 15, t, kText);
    return w;
}

constexpr float kListRows = 5, kListRowH = 32, kGap = 8, kGroupPad = 10, kGroupHead = 34, kGroupGap = 12;

float control_height(Kind kind) {
    switch (kind) {
    case Kind::radio:
    case Kind::slider: return 84;
    case Kind::list: return 40 + kListRows * kListRowH + 14;
    case Kind::button: return 48;
    default: return 54;
    }
}

float group_height(Span page, const Group &g) {
    float h = 2 * kGroupPad + kGroupHead;
    for (int i = g.first; i < g.first + g.count; ++i) h += control_height(page[i].kind) + (i > g.first ? kGap : 0);
    return h;
}

// the tallest column of any tab (the panel keeps one size for all three)
float columns_height() {
    float most = 0;
    for (int t = 0; t < 3; ++t) {
        float col[2] = {0, 0};
        for (const Group &g : groups_of(t)) col[g.column] += (col[g.column] > 0 ? kGroupGap : 0) + group_height(page_controls(Page::settings, t), g);
        most = std::max({most, col[0], col[1]});
    }
    return most;
}

}  // namespace

void Menu::add_hit(const Picture &out, float x, float y, float w, float h, int control, int choice, int tab, int key_row, int cell) {
    Hit hit;
    hit.box = {out.x + static_cast<int>(std::floor(x)), out.y + static_cast<int>(std::floor(y)), std::max(1, static_cast<int>(w)), std::max(1, static_cast<int>(h))};
    hit.control = control;
    hit.choice = choice;
    hit.tab = tab;
    hit.key_row = key_row;
    hit.cell = cell;
    hits_.push_back(hit);
}

void Menu::draw(int window_width, int window_height, const Values &values, Picture &out) {
    hits_.clear();
    window_width = std::max(1, window_width);
    window_height = std::max(1, window_height);
    panel_w_ = page_ == Page::main ? 560.0f : page_ == Page::settings ? 1100.0f : 1000.0f;  // (as the draw functions below lay out)
    if (page_ == Page::keys || page_ == Page::pad) draw_keys_page(window_width, window_height, values, out);
    else if (page_ == Page::states) draw_states_page(window_width, window_height, values, out);
    else if (page_ == Page::cards) draw_cards_page(window_width, window_height, values, out);
    else if (page_ == Page::mods) draw_mods_page(window_width, window_height, values, out);
    else if (page_ == Page::install) draw_install_page(window_width, window_height, values, out);
    else if (page_ == Page::pick_iso) draw_picker_page(window_width, window_height, values, out);
    else if (page_ == Page::progress) draw_progress_page(window_width, window_height, values, out);
    else if (list_page()) draw_list_page(window_width, window_height, values, out);
    else if (page_ == Page::main) draw_main(window_width, window_height, out);
    else draw_settings(window_width, window_height, values, out);
    if (page_ != Page::main && page_ != Page::install && page_ != Page::progress) draw_back_button(out);  // local change: for the mouse
    if (editing_) draw_keyboard(out);
    if (dialog_.open) draw_dialog(out);
}

// local change: a Back button at the top left of the panel, beside the title tab (every page has its body at 46 units): for the mouse,
// what Triangle / Esc does there (see back_click)
void Menu::draw_back_button(Picture &out) {
    const float k = static_cast<float>(out.width) / std::max(1.0f, panel_w_);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    constexpr float x = 10, y = 6, w = 118, h = 34;
    p.rrect(x, y, w, h, h / 2, kTile, 2, kBorder);
    p.tri(x + 16, y + h / 2, x + 28, y + 9, x + 28, y + h - 9, kBorder);
    p.text_centre(x + 26, y, w - 30, h, 19, "Back", kText, true);
    add_hit(out, p.S(x), p.S(y), p.S(w), p.S(h), -1, -1, -1, -1);
    hits_.back().back = true;
}

Box Menu::back_box() const {
    for (const Hit &h : hits_)
        if (h.back) return h.box;
    return {};
}

// the Back button: what Triangle does, except that it leaves the page in one click - the settings page even from inside a category
// or an open list (its question about changes not applied still comes), the picker at once (Triangle there goes a folder up)
Action Menu::back_click(const Values &v) {
    status_.clear();
    if (page_ == Page::pick_iso) return {Command::picker_open, -2};
    if (page_ == Page::settings) {
        list_open_ = false;
        inside_ = false;
        return press_settings(Input::back, v);
    }
    if (capturing_) capturing_ = false;
    return press(Input::back, v);
}

void Menu::draw_main(int ww, int wh, Picture &out) {
    const Span page = main_controls(mode_, !update_.empty(), !hd_label_.empty());
    const bool launcher = mode_ == Mode::launcher;
    constexpr float W = 560, top = 46, tile_h = 52, gap = 9, x = 24;
    const float status_h = main_status_.empty() ? 0.0f : 52.0f;
    const float H = top + 30 + page.n * (tile_h + gap) + status_h + 8 + 66 + 56;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    const char *title = launcher ? "The Sims 2" : "Menu";
    title_tab(p, W, top, title);
    body(p, W, H, top);
    join_title_tab(p, W, top, title);
    float y = top + 30;
    for (int i = 0; i < page.n; ++i) {
        const Control &c = page[i];
        const bool lit = i == focus_ && !c.disabled;
        p.rrect(x, y, W - 2 * x, tile_h, 10, c.disabled ? kTileOff : lit ? kSelect : kTile, 2, c.disabled ? kDisabled : kBorder);
        std::string label = c.label;
        if (c.cmd == armed_) label = c.cmd == Command::quit ? "Really quit?  Cross again" : "Really restart?  Cross again";
        if (c.cmd == Command::update_now) label = "Update to " + update_;
        if (c.cmd == Command::get_hd) label = hd_label_;
        p.text_in(x + 18, y, tile_h, W - 2 * x - 140, 24, label, c.disabled ? kDisabled : kText, lit);
        if (c.disabled) p.text_right(W - x - 16, y, tile_h, 16, "coming later", kDisabled);
        else if (*c.key) p.text_right(W - x - 16, y, tile_h, 18, c.key, kSoftText);
        add_hit(out, p.S(x), p.S(y), p.S(W - 2 * x), p.S(tile_h), i, -1, -1, -1);
        y += tile_h + gap;
    }
    if (!main_status_.empty()) {  // the launcher's line: the card slots, or why Play did not start
        p.paragraph(x + 6, y + 4, W - 2 * x - 12, 17, main_status_, kText, 2);
        y += status_h;
    }
    y += 8;
    p.rrect(x, y, W - 2 * x, 58, 8, kHelp);
    p.paragraph(x + 12, y + 6, W - 2 * x - 24, 17, page[focus_].help, kText, 2);
    p.text_centre(30, H - 50, W - 60, 36, 18, launcher ? "Cross: choose      Triangle / Esc: quit" : "Cross: choose      Triangle: back to the game", kSoftText);
}

void Menu::draw_settings(int ww, int wh, const Values &v, Picture &out) {
    constexpr float W = 1100, top = 46, inner_top = 134, col_gap = 20, button_h = 50, help_h = 60;
    const float groups_y = inner_top + 64;
    const float buttons_y = groups_y + columns_height() + 14;
    const float help_y = buttons_y + button_h + 10;
    const float inner_bottom = help_y + help_h + 12;
    const float H = inner_bottom + 54;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    title_tab(p, W, top, "Settings");
    body(p, W, H, top);
    join_title_tab(p, W, top, "Settings");
    // the inner panel, then the tabs over its top edge
    p.rrect(24, inner_top, W - 48, inner_bottom - inner_top, 14, kInner, 2.5f);
    // L1 / R1 badges with their arrows; a click goes one tab left or right
    p.tri(24, 95, 44, 76, 44, 114, kBorder);
    p.rrect(50, 76, 56, 38, 6, kBadge);
    p.text_centre(50, 76, 56, 38, 22, "L1", kWhite, true);
    p.tri(W - 24, 95, W - 44, 76, W - 44, 114, kBorder);
    p.rrect(W - 106, 76, 56, 38, 6, kBadge);
    p.text_centre(W - 106, 76, 56, 38, 22, "R1", kWhite, true);
    add_hit(out, p.S(20), p.S(70), p.S(90), p.S(50), -1, -1, (tab_ + 2) % 3, -1);
    add_hit(out, p.S(W - 110), p.S(70), p.S(90), p.S(50), -1, -1, (tab_ + 1) % 3, -1);
    constexpr float tab_w = 104, tab_gap = 6;
    const float tabs_x = (W - 3 * tab_w - 2 * tab_gap) / 2;
    for (int t = 0; t < 3; ++t) {
        const bool on = t == tab_;
        const float tx = tabs_x + t * (tab_w + tab_gap), ty = on ? 56 : 64;
        p.rrect(tx, ty, tab_w, inner_top + 8 - ty, 14, on ? kTabOn : kTabOff, 2.5f);
        if (on) p.c.fill_rect(p.S(tx + 2.5f), p.S(inner_top - 3), p.S(tab_w - 5), p.S(9), kTabOn);
        // (opaque colours: the icons' line pieces overlap, a see-through colour would show darker joins)
        const std::uint32_t col = on ? kDarkGreen : hex(0x3A4590);
        const float s = 50, ix = tx + (tab_w - s) / 2, iy = ty + 9;
        if (t == 0) icon_monitor(p, ix, iy, s, col);
        else if (t == 1) icon_hourglass(p, ix, iy, s, col);
        else icon_controller(p, ix, iy, s, col, on ? hex(0xAAB8EC) : hex(0x8193D9));
        add_hit(out, p.S(tx), p.S(ty), p.S(tab_w), p.S(inner_top - ty), -1, -1, t, -1);
    }
    static const char *const kStripText[3] = {"Graphics options", "Game options", "Controls"};
    p.rrect(44, inner_top + 14, W - 88, 40, 6, kStrip);
    p.text_centre(44, inner_top + 14, W - 88, 40, 26, kStripText[tab_], kText, true);

    // the categories, two columns side by side
    const float col_w = (W - 88 - col_gap) / 2;
    float col_y[2] = {groups_y, groups_y};
    const Span page = page_controls(page_, tab_);
    const std::vector<Group> gs = groups_of(tab_);
    const int G = static_cast<int>(gs.size());
    const char *help = "";
    for (int g = 0; g < G; ++g) {
        const Group &grp = gs[g];
        const float gx = 44 + grp.column * (col_w + col_gap), gy = col_y[grp.column], gh = group_height(page, grp);
        col_y[grp.column] += gh + kGroupGap;
        const bool here = g == cell_;
        const bool lit = here && !inside_;
        p.rrect(gx, gy, col_w, gh, 12, lit ? kGroupLit : kGroup, lit || (here && inside_) ? 3.5f : 2, here ? kDarkGreen : kBorder);
        p.text_centre(gx, gy + kGroupPad - 2, col_w, kGroupHead, 24, grp.title, kText, true);
        if (lit) help = grp.help;
        float y = gy + kGroupPad + kGroupHead;
        const float x = gx + 10, w = col_w - 20;
        for (int i = grp.first; i < grp.first + grp.count; ++i) {
            const Control &c = page[i];
            const Setting &s = setting_of(v, c.cmd);
            const int n = static_cast<int>(s.choices.size());
            const int chosen = std::clamp(shown(c.cmd, v), 0, std::max(0, n - 1));
            const bool focused = inside_ && i == row_, inside_list = focused && list_open_;
            const bool changed = pending_[static_cast<int>(c.cmd)] >= 0;
            if (focused) help = c.help;
            const float h = control_height(c.kind);
            p.rrect(x, y, w, h, 8, focused && !inside_list ? kSelect : kTile, focused ? 2.5f : 1.5f, focused ? kDarkGreen : kBorder);
            const bool game_only = mode_ == Mode::launcher && needs_game_window(c.cmd);
            const std::uint32_t label_col = s.locked || game_only ? kDisabled : kText;
            std::string label = c.label;
            if (s.locked && !s.locked_note.empty()) label += "  (" + s.locked_note + ")";
            if (game_only) label += "  (in the game's quick menu)";
            const float label_h = c.kind == Kind::checkbox || c.kind == Kind::button ? h : 36;
            const float label_x = c.kind == Kind::checkbox ? x + 54 : x + 14;
            const int value_at = focused && c.kind == Kind::slider ? std::clamp(hl_, 0, std::max(0, n - 1)) : chosen;
            const bool value_right = c.kind == Kind::slider && n > 0;
            const float value_w = value_right ? std::min(w * 0.4f, p.width(s.choices[value_at], 19) + 4) : 0;
            const std::string tag_text = changed ? "not applied" : s.next_start ? "needs restart" : "";
            const float tag_w = tag_text.empty() ? 0 : tag_width(p, tag_text) + 10;
            if (c.kind == Kind::button) {
                p.text_centre(x + 10, y, w - 20, h, 22, label, label_col, true);
            } else {
                const float room = w - (label_x - x) - 14 - value_w - tag_w;
                const std::string fitted = menufont::fit_text(p.f, label, p.S(22), p.S(room));
                p.text_in(label_x, y + 2, label_h - 2, room, 22, label, label_col, focused);
                if (!tag_text.empty()) tag(p, label_x + p.width(fitted, 22) + 10, y + 2, label_h - 2, tag_text, changed ? kWhite : kAmber);
            }
            switch (c.kind) {
            case Kind::checkbox: {
                const float bx = x + 14, by = y + (h - 28) / 2;
                p.rrect(bx, by, 28, 28, 4, kBox, 2);
                if (chosen) {
                    p.stroke(bx + 7, by + 7, bx + 21, by + 21, 3.5f, kBorder);
                    p.stroke(bx + 21, by + 7, bx + 7, by + 21, 3.5f, kBorder);
                }
                break;
            }
            case Kind::radio: {
                if (n == 0) break;
                const float slot = (w - 28) / static_cast<float>(n), cy = y + 60;
                for (int ch = 0; ch < n; ++ch) {
                    const float sx = x + 14 + ch * slot;
                    if (focused && ch == hl_) p.rrect(sx - 4, cy - 17, slot - 4, 34, 9, kBox, 2.5f, kDarkGreen);  // where the pad is
                    p.circle(sx + 12, cy, 10, kBox, 2);
                    if (ch == chosen) p.circle(sx + 12, cy, 5, s.locked ? kDisabled : kBorder);
                    p.text_in(sx + 28, cy - 16, 32, slot - 36, 19, s.choices[ch], label_col, ch == chosen);
                    add_hit(out, p.S(sx), p.S(cy - 20), p.S(slot), p.S(40), i, ch, -1, -1);
                }
                break;
            }
            case Kind::slider: {
                if (n == 0) break;
                p.text_right(x + w - 14, y + 2, 34, 19, menufont::fit_text(p.f, s.choices[value_at], p.S(19), p.S(value_w)),
                             focused && value_at != chosen ? kDarkGreen : label_col);
                const float x0 = x + 28, x1 = x + w - 28, ty = y + 60;
                const float step = n > 1 ? (x1 - x0) / static_cast<float>(n - 1) : 0;
                const float kx = x0 + step * chosen;
                p.stroke(x0, ty, x1, ty, 9, kBorder);
                p.stroke(x0, ty, x1, ty, 4.5f, kBox);
                if (focused) p.stroke(x0, ty, kx, ty, 4.5f, kGreen);
                for (int st = 0; st < n; ++st) p.circle(x0 + step * st, ty, 2.2f, kBorder);
                p.rrect(kx - 8, ty - 16, 16, 32, 6, kBorder);
                p.rrect(kx - 4, ty - 12, 8, 24, 3, focused ? kGreen : kBox);
                if (focused && hl_ != chosen) {  // the highlighted step, picked with Cross
                    const float hx = x0 + step * std::clamp(hl_, 0, n - 1);
                    p.c.stroke_round_rect(p.S(hx - 10), p.S(ty - 18), p.S(20), p.S(36), p.S(7), p.S(3), kDarkGreen);
                }
                for (int st = 0; st < n; ++st) {
                    const float half = n > 1 ? step / 2 : (x1 - x0) / 2;
                    add_hit(out, p.S(x0 + step * st - half), p.S(ty - 20), p.S(2 * half), p.S(40), i, st, -1, -1);
                }
                break;
            }
            case Kind::list: {
                const float bx = x + 14, by = y + 40, bw = w - 28, bh = kListRows * kListRowH + 8;
                p.rrect(bx, by, bw, bh, 6, kBox, inside_list ? 2.5f : 2, inside_list ? kDarkGreen : kBorder);
                const int rows = static_cast<int>(kListRows);
                const int anchor = inside_list ? list_hl_ : chosen;
                int top_row = inside_list ? list_top_ : 0;
                if (anchor < top_row) top_row = anchor;
                if (anchor >= top_row + rows) top_row = anchor - rows + 1;
                top_row = std::clamp(top_row, 0, std::max(0, n - rows));
                if (inside_list) list_top_ = top_row;
                if (n > rows) {  // the scroll bar
                    const float sx = bx + bw - 16, sy = by + 6, sh = bh - 12;
                    p.rrect(sx, sy, 10, sh, 5, kScroll, 1.5f);
                    const float th = std::max(14.0f, sh * rows / n), tyy = sy + (sh - th) * top_row / static_cast<float>(n - rows);
                    p.rrect(sx, tyy, 10, th, 5, kBorder);
                }
                for (int r = 0; r < rows && top_row + r < n; ++r) {
                    const int ch = top_row + r;
                    const float ry = by + 4 + r * kListRowH, rw = bw - (n > rows ? 28 : 8);
                    if (inside_list && ch == list_hl_) p.rrect(bx + 4, ry, rw, kListRowH, 6, kSelect, 2, kDarkGreen);
                    if (ch == chosen) p.circle(bx + 16, ry + kListRowH / 2, 4.5f, kBorder);
                    p.text_in(bx + 28, ry, kListRowH, rw - 30, 19, s.choices[ch], kText, ch == chosen);
                    if (inside_list) add_hit(out, p.S(bx + 4), p.S(ry), p.S(rw), p.S(kListRowH), -1, ch, -1, -1);
                }
                break;
            }
            default: break;
            }
            add_hit(out, p.S(x), p.S(y), p.S(w), p.S(h), i, -1, -1, -1);
            y += h + kGap;
        }
        add_hit(out, p.S(gx), p.S(gy), p.S(col_w), p.S(gh), -1, -1, -1, -1, g);
    }

    // the buttons below the categories
    const int changes = change_count();
    const int first = first_button(), shown_buttons = kActionCount - first;
    const float bw = (W - 88 - (shown_buttons - 1) * 14) / static_cast<float>(shown_buttons);
    for (int b = first; b < kActionCount; ++b) {
        const float bx = 44 + (b - first) * (bw + 14);
        const bool lit = !inside_ && cell_ == G + b;
        const bool idle = kButtons[b].cmd == Command::apply && changes == 0;
        if (lit) help = kButtons[b].help;
        p.rrect(bx, buttons_y, bw, button_h, 10, lit ? kSelect : idle ? kTileOff : kTile, lit ? 3 : 2, lit ? kDarkGreen : idle ? kDisabled : kBorder);
        std::string label = kButtons[b].label;
        if (kButtons[b].cmd == Command::apply && changes) label += " (" + std::to_string(changes) + ")";
        p.text_centre(bx + 6, buttons_y, bw - 12, button_h, 21, label, idle ? kDisabled : kText, true);
        add_hit(out, p.S(bx), p.S(buttons_y), p.S(bw), p.S(button_h), -1, -1, -1, -1, G + b);
    }

    // what the focused thing does
    p.rrect(44, help_y, W - 88, help_h, 8, kHelp);
    if (discard_armed_) p.paragraph(58, help_y + 7, W - 116, 18, "Your changes are not applied yet. Triangle again throws them away; Apply changes keeps them.", hex(0xA04010), 2);
    else if (!status_.empty()) p.paragraph(58, help_y + 7, W - 116, 18, status_, kDarkGreen, 2);
    else p.paragraph(58, help_y + 7, W - 116, 18, help, kText, 2);

    const char *hint = list_open_ ? "Up / down: highlight    Cross: pick    Triangle: cancel"
                       : inside_  ? "Up / down: setting    Left / right: choose    Cross: pick    Triangle: back"
                                  : "D-pad: move    Cross: open    L1 / R1: tabs    Triangle: back";
    p.text_centre(40, inner_bottom + 8, W - 80, 38, 19, hint, kSoftText);
}

void Menu::draw_list_page(int ww, int wh, const Values &v, Picture &out) {
    const bool cheats = page_ == Page::cheats;
    fit_list(v);
    constexpr int kShown = 13;
    keys_shown_ = kShown;
    fit_list(v);
    const int shown = std::min(key_rows_, kShown);
    constexpr float W = 1000, top = 46, row_h = 34, row_gap = 4, help_h = 60;
    const float rows_y = top + 90;
    const float H = rows_y + kShown * (row_h + row_gap) + 10 + row_h + 16 + help_h + 54;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    const std::string title = cheats ? "Cheats" : "Debug";
    title_tab(p, W, top, title);
    body(p, W, H, top);
    join_title_tab(p, W, top, title);
    p.rrect(24, top + 24, W - 48, 40, 6, kStrip);
    p.text_in(40, top + 24, 40, W - 260, 21,
              cheats ? "Used at once, until the game restarts; gnome cheats appear on the gnome on your lot" : "The developers' settings - used at once, until the game restarts (some act only on the next lot)",
              kText, true);
    if (key_rows_ > shown)
        p.text_right(W - 40, top + 24, 40, 18, std::to_string(key_top_ + 1) + "-" + std::to_string(key_top_ + shown) + " of " + std::to_string(key_rows_), kSoftText);
    const char *help = "";
    float y = rows_y;
    for (int r = 0; r < shown; ++r) {
        const int index = key_top_ + r;
        const ListRow &row = v.list_rows[index];
        if (row.header) {
            p.text_in(32, y + 4, row_h - 4, W - 64, 21, row.label, kText, true);
            p.stroke(32, y + row_h - 3, W - 32, y + row_h - 3, 1.5f, hex(0x1E2A78, 120));
            y += row_h + row_gap;
            continue;
        }
        const bool lit = index == key_sel_;
        if (lit) help = row.help.c_str();
        p.rrect(24, y, W - 48, row_h, 7, lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
        float lx = 40;
        if (row.checkbox) {
            const float bx = 36, by = y + (row_h - 22) / 2;
            p.rrect(bx, by, 22, 22, 3, kBox, 2);
            if (row.on) {
                p.stroke(bx + 5, by + 5, bx + 17, by + 17, 3, kBorder);
                p.stroke(bx + 17, by + 5, bx + 5, by + 17, 3, kBorder);
            }
            lx = 70;
        }
        float right = W - 40;
        if (row.action) {  // a small button with its word ("Play")
            const float bw = p.width(row.value, 17) + 28;
            p.rrect(right - bw, y + 5, bw, row_h - 10, (row_h - 10) / 2, lit ? kWhite : kBox, 1.5f, kBorder);
            p.text_centre(right - bw, y + 5, bw, row_h - 10, 17, row.value, kText, true);
            right -= bw + 14;
        }
        if (row.number) {
            const std::string val = "<  " + row.value + "  >";
            p.text_right(right, y, row_h, 19, val, lit ? kDarkGreen : kText);
            right -= p.width(val, 19) + 14;
        }
        if (row.changed) {
            const float tw = tag_width(p, "set here");
            tag(p, right - tw, y, row_h, "set here", kWhite);
            right -= tw + 10;
        }
        p.text_in(lx, y, row_h, right - lx - 8, 20, row.label, kText, lit);
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, index);
        y += row_h + row_gap;
    }
    y = rows_y + kShown * (row_h + row_gap) + 10;
    {
        const bool lit = key_sel_ == key_rows_;
        p.rrect(24, y, W - 48, row_h, 7, lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
        const std::string text = defaults_armed_ ? "Really put everything back to the game's own values?  Cross again"
                                                 : "Put everything back to the game's own values";
        p.text_centre(40, y, W - 80, row_h, 20, text, kText, lit);
        if (lit) help = cheats ? "Turns every cheat on this page off again (the gnome cheats and the hidden ones)."
                               : "Puts every setting changed here back to the game's own value now (a restart does that too).";
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, key_rows_);
    }
    y += row_h + 16;
    p.rrect(24, y, W - 48, help_h, 8, kHelp);
    if (!status_.empty()) p.paragraph(38, y + 7, W - 76, 18, status_, kDarkGreen, 2);
    else p.paragraph(38, y + 7, W - 76, 18, help, kText, 2);
    p.text_centre(40, H - 50, W - 80, 36, 18, "Up / down: choose    Cross: switch    Left / right: number    L1 / R1: page    Triangle: back", kSoftText);
}

void Menu::draw_states_page(int ww, int wh, const Values &v, Picture &out) {
    constexpr float W = 1000, top = 46, row_h = 40, row_gap = 6, help_h = 60;
    const float rows_y = top + 90;
    const float H = rows_y + (row_h + 16) + 10 * (row_h + row_gap) + 10 + help_h + 54;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    title_tab(p, W, top, "Save states");
    body(p, W, H, top);
    join_title_tab(p, W, top, "Save states");
    p.rrect(24, top + 24, W - 48, 40, 6, kStrip);
    p.text_in(40, top + 24, 40, W - 80, 21, "Load at the top; choose a slot below to save the game into it", kText, true);
    bool any = false;
    for (const Values::Slot &s : v.slots) any = any || s.used;
    if (any && (load_slot_ < 1 || load_slot_ > 10 || !v.slots[load_slot_ - 1].used))
        for (int s = 1; s <= 10; ++s)
            if (v.slots[s - 1].used) {
                load_slot_ = s;
                break;
            }
    const char *help = "";
    float y = rows_y;
    {  // the Load row
        const bool lit = key_sel_ == 0;
        p.rrect(24, y, W - 48, row_h, 8, lit ? kSelect : any ? kTile : kTileOff, lit ? 3 : 2, lit ? kDarkGreen : kBorder);
        p.text_in(44, y, row_h, 200, 23, "Load", any ? kText : kDisabled, true);
        const Values::Slot *ls = any ? &v.slots[load_slot_ - 1] : nullptr;
        const std::string what = !any ? "No save states yet"
                                      : (ls->name.empty() ? "Slot " + std::to_string(load_slot_) : std::to_string(load_slot_) + ": " + ls->name) + "   " + ls->when;
        p.text_centre(260, y, W - 520, row_h, 21, what, any ? kText : kDisabled, lit);
        if (any) {
            p.tri(250, y + row_h / 2, 262, y + 10, 262, y + row_h - 10, lit ? kDarkGreen : kBorder);
            p.tri(W - 250, y + row_h / 2, W - 262, y + 10, W - 262, y + row_h - 10, lit ? kDarkGreen : kBorder);
        }
        if (lit) help = any ? "Left / right: choose which save state to load. Cross: load it." : "Save the game into a slot below first.";
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, 0);
        y += row_h + 16;
    }
    for (int s = 1; s <= 10; ++s) {
        const bool lit = key_sel_ == s;
        const Values::Slot &slot = v.slots[s - 1];
        p.rrect(24, y, W - 48, row_h, 8, lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
        const bool on_rename = lit && slot_col_ == 1;
        p.text_in(44, y, row_h, 130, 21, "Slot " + std::to_string(s), kText, lit && !on_rename);
        if (slot.used) {
            p.text_in(180, y, row_h, 420, 20, slot.name.empty() ? std::string("(no name)") : slot.name, slot.name.empty() ? kSoftText : kText, !slot.name.empty());
            p.text_in(610, y, row_h, 230, 18, slot.when, kSoftText);
            // the Rename button at the right end of the row
            const float bx = W - 24 - 128, bw = 120;
            p.rrect(bx, y + 5, bw, row_h - 10, (row_h - 10) / 2, on_rename ? kWhite : kBox, on_rename ? 2.5f : 1.5f, on_rename ? kDarkGreen : kBorder);
            p.text_centre(bx, y + 5, bw, row_h - 10, 17, "Rename", kText, on_rename);
        } else {
            p.text_in(180, y, row_h, W - 260, 20, "empty", kDisabled);
        }
        if (lit) help = on_rename ? "Cross: give this slot a name (keyboard or controller)."
                      : slot.used ? "Cross: save the game as it is now into this slot (it asks before saving over it). Right: Rename."
                                  : "Cross: save the game as it is now into this slot.";
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, s);
        y += row_h + row_gap;
    }
    y += 10;
    p.rrect(24, y, W - 48, help_h, 8, kHelp);
    if (!status_.empty()) p.paragraph(38, y + 7, W - 76, 18, status_, kDarkGreen, 2);
    else p.paragraph(38, y + 7, W - 76, 18, help, kText, 2);
    p.text_centre(40, H - 50, W - 80, 36, 18, "Up / down: choose    Left / right: slot to load / Rename    Cross: load / save    Triangle: back", kSoftText);
}

void Menu::draw_cards_page(int ww, int wh, const Values &v, Picture &out) {
    constexpr float W = 1000, top = 46, row_h = 40, row_gap = 6, help_h = 60;
    const int n = static_cast<int>(v.cards.size());
    cards_shown_ = 7;
    const float rows_y = top + 90;
    const float H = rows_y + 3 * (row_h + row_gap) + 16 + cards_shown_ * (row_h + row_gap) + 26 + help_h + 54;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    title_tab(p, W, top, "Memory cards");
    body(p, W, H, top);
    join_title_tab(p, W, top, "Memory cards");
    p.rrect(24, top + 24, W - 48, 40, 6, kStrip);
    p.text_in(40, top + 24, 40, W - 80, 21, "The cards in the slots at the top; every card you have below", kText, true);
    float y = rows_y;
    std::string help;
    if (!v.card_library) {
        p.paragraph(44, y, W - 88, 21, "This run has no card library: the launcher gives the memory card with --memcard (and --memcard2). Start the game "
                                        "with play-sims2.cmd to use named cards.", kText, 3);
    } else {
        const int rows = kCardFirstRow + n;
        key_sel_ = std::clamp(key_sel_, 0, rows - 1);
        key_top_ = std::clamp(key_top_, 0, std::max(0, n - cards_shown_));
        for (int s = 0; s < 2; ++s) {  // the slots
            const bool lit = key_sel_ == s;
            const std::string &name = v.slot_cards[s];
            p.rrect(24, y, W - 48, row_h, 8, lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
            p.text_in(44, y, row_h, 130, 21, "Slot " + std::to_string(s + 1), kText, true);
            p.text_in(180, y, row_h, 560, 21, name.empty() ? std::string("empty") : name, name.empty() ? kDisabled : kText, !name.empty());
            if (!name.empty()) {
                const float bx = W - 24 - 128, bw = 120;
                p.rrect(bx, y + 5, bw, row_h - 10, (row_h - 10) / 2, lit ? kWhite : kBox, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
                p.text_centre(bx, y + 5, bw, row_h - 10, 17, "Take out", kText, lit);
            }
            if (lit) help = name.empty() ? "Empty. Choose a card below and press its \"Slot " + std::to_string(s + 1) + "\" button to put it in."
                                         : "Cross: take this card out of the slot (the game sees the slot empty).";
            add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, s);
            y += row_h + row_gap;
        }
        {  // New card
            const bool lit = key_sel_ == 2;
            p.rrect(24, y, W - 48, row_h, 8, lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
            p.text_centre(24, y, W - 48, row_h, 21, "+  New card", kText, true);
            if (lit) help = "Cross: make a new, empty card with a random town name (rename it later). The game formats it when you first save on it.";
            add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, 2);
            y += row_h + row_gap + 16;
        }
        static const char *const kColumns[kCardColumns] = {"Slot 1", "Slot 2", "Rename", "Copy", "Delete"};
        constexpr float bw = 88, bgap = 6, buttons_w = kCardColumns * bw + (kCardColumns - 1) * bgap;
        const float bx0 = W - 24 - 8 - buttons_w;
        if (n == 0) p.text_in(44, y, row_h, W - 88, 20, "No cards yet: make one with \"New card\".", kDisabled);
        for (int i = key_top_; i < std::min(n, key_top_ + cards_shown_); ++i) {
            const Values::Card &c = v.cards[i];
            const int row = kCardFirstRow + i;
            const bool lit = key_sel_ == row;
            p.rrect(24, y, W - 48, row_h, 8, lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
            p.text_in(44, y, row_h, bx0 - 44 - 150, 20, c.name, kText, true);
            const std::string tag = c.slot ? "in slot " + std::to_string(c.slot) : c.when.empty() ? "new" : c.when;
            p.text_in(bx0 - 150, y, row_h, 144, 16, tag, c.slot ? kDarkGreen : kSoftText, c.slot != 0);
            for (int b = 0; b < kCardColumns; ++b) {
                const float bx = bx0 + b * (bw + bgap);
                const bool on = lit && card_col_ == b;
                const bool off = (b < 2 && c.slot != 0) || (b == 4 && c.slot != 0);
                p.rrect(bx, y + 5, bw, row_h - 10, (row_h - 10) / 2, on ? kWhite : off ? kTileOff : kBox, on ? 2.5f : 1.5f, on ? kDarkGreen : off ? kDisabled : kBorder);
                p.text_centre(bx, y + 5, bw, row_h - 10, 16, kColumns[b], off ? kDisabled : kText, on);
                add_hit(out, p.S(bx), p.S(y + 5), p.S(bw), p.S(row_h - 10), -1, b, -1, row);
            }
            if (lit) {
                switch (card_col_) {
                case 0:
                case 1:
                    help = c.slot ? "This card is in slot " + std::to_string(c.slot) + "."
                                  : "Cross: put this card into slot " + std::to_string(card_col_ + 1) + " (the card there now comes out). Not while the game is loading or saving.";
                    break;
                case 2: help = "Cross: give this card a new name (keyboard or controller)."; break;
                case 3: help = "Cross: make a copy of this card with all its saves, under a new name."; break;
                default: help = c.slot ? "Take the card out of its slot first, then delete it." : "Cross: delete this card for good, with every save on it (it asks first)."; break;
                }
            }
            add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, row);
            y += row_h + row_gap;
        }
        for (int i = std::min(n, key_top_ + cards_shown_) - key_top_; i < cards_shown_; ++i) y += row_h + row_gap;
        if (n > cards_shown_)
            p.text_centre(24, y, W - 48, 22, 16, "cards " + std::to_string(key_top_ + 1) + " to " + std::to_string(std::min(n, key_top_ + cards_shown_)) + " of " +
                                                  std::to_string(n) + "   (L1 / R1: page)", kSoftText);
        y += 26;
    }
    p.rrect(24, y, W - 48, help_h, 8, kHelp);
    if (!status_.empty()) p.paragraph(38, y + 7, W - 76, 18, status_, kDarkGreen, 2);
    else p.paragraph(38, y + 7, W - 76, 18, help, kText, 2);
    p.text_centre(40, H - 50, W - 80, 36, 18, "Up / down: choose    Left / right: button    Cross: do it    L1 / R1: page    Triangle: back", kSoftText);
}

// the launcher's Mods page (local change, stage 3)
void Menu::draw_mods_page(int ww, int wh, const Values &v, Picture &out) {
    constexpr float W = 1000, top = 46, row_h = 40, row_gap = 6, help_h = 128;
    const int n = static_cast<int>(v.mods.rows.size());
    mods_shown_ = 6;
    const float rows_y = top + 90;
    const float H = rows_y + (row_h + row_gap) + 16 + mods_shown_ * (row_h + row_gap) + 26 + help_h + 54;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    title_tab(p, W, top, "Mods");
    body(p, W, H, top);
    join_title_tab(p, W, top, "Mods");
    p.rrect(24, top + 24, W - 48, 40, 6, kStrip);
    p.text_in(40, top + 24, 40, W - 80, 20, "Texture packs, settings and patches. Higher in the list wins. Used when the game starts.", kText, true);
    const int rows = kModFirstRow + n;
    key_sel_ = std::clamp(key_sel_, 0, rows - 1);
    key_top_ = std::clamp(key_top_, 0, std::max(0, n - mods_shown_));
    float y = rows_y;
    std::string help;
    std::vector<std::string> warnings;
    {  // Install from zip
        const bool lit = key_sel_ == 0;
        p.rrect(24, y, W - 48, row_h, 8, lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
        p.text_centre(24, y, W - 48, row_h, 20, "+  Install a mod from a .zip", kText, true);
        if (lit) help = "Cross: choose a mod's .zip; it is unpacked into the mods folder and turned on. (A .zip put into the mods folder by hand is unpacked by itself.)";
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, 0);
        y += row_h + row_gap + 16;
    }
    static const char *const kColumns[kModColumns] = {"On", "Up", "Down", "Delete"};
    constexpr float bw = 80, bgap = 6, buttons_w = kModColumns * bw + (kModColumns - 1) * bgap;
    const float bx0 = W - 24 - 8 - buttons_w;
    if (n == 0) p.text_in(44, y, row_h, W - 88, 20, "No mods yet: install one from a zip. (The HD textures are part of the game: Settings > Textures.)", kDisabled);
    for (int i = key_top_; i < std::min(n, key_top_ + mods_shown_); ++i) {
        const Values::ModRow &m = v.mods.rows[static_cast<std::size_t>(i)];
        const int row = kModFirstRow + i;
        const bool lit = key_sel_ == row;
        p.rrect(24, y, W - 48, row_h, 8, lit ? kSelect : m.on ? kTile : kTileOff, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
        p.text_in(44, y, row_h, 30, 18, std::to_string(i + 1) + ".", kSoftText);
        p.text_in(76, y, row_h, bx0 - 76 - 170, 20, m.name, m.on ? kText : kSoftText, m.on);
        const std::string tag = !m.can_on ? "another game" : !m.warnings.empty() ? std::to_string(m.warnings.size()) + (m.warnings.size() == 1 ? " warning" : " warnings") : m.on ? "on" : "off";
        p.text_in(bx0 - 170, y, row_h, 164, 16, tag, !m.warnings.empty() || !m.can_on ? kAmber : m.on ? kDarkGreen : kSoftText, m.on);
        for (int b = 0; b < kModColumns; ++b) {
            const float bx = bx0 + b * (bw + bgap);
            const bool on = lit && card_col_ == b;
            const bool off = (b == 0 && !m.on && !m.can_on) || (b == 1 && i == 0) || (b == 2 && i == n - 1);
            const std::string word = b == 0 ? (m.on ? "Turn off" : "Turn on") : kColumns[b];
            p.rrect(bx, y + 5, bw, row_h - 10, (row_h - 10) / 2, on ? kWhite : off ? kTileOff : kBox, on ? 2.5f : 1.5f, on ? kDarkGreen : off ? kDisabled : kBorder);
            p.text_centre(bx, y + 5, bw, row_h - 10, 15, word, off ? kDisabled : kText, on);
            add_hit(out, p.S(bx), p.S(y + 5), p.S(bw), p.S(row_h - 10), -1, b, -1, row);
        }
        if (lit) {
            help = m.name + (m.info.empty() ? "" : "  -  " + m.info) + "\n" + m.details;
            warnings = m.warnings;
        }
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, row);
        y += row_h + row_gap;
    }
    for (int i = std::min(n, key_top_ + mods_shown_) - key_top_; i < mods_shown_; ++i) y += row_h + row_gap;
    if (n > mods_shown_)
        p.text_centre(24, y, W - 48, 22, 16, "mods " + std::to_string(key_top_ + 1) + " to " + std::to_string(std::min(n, key_top_ + mods_shown_)) + " of " + std::to_string(n) +
                                              "   (L1 / R1: page)", kSoftText);
    y += 26;
    p.rrect(24, y, W - 48, help_h, 8, kHelp);
    if (!status_.empty()) p.paragraph(38, y + 7, W - 76, 18, status_, kDarkGreen, 5);
    else {
        std::string text = help;
        for (const std::string &w : warnings) text += "\n! " + w;
        p.paragraph(38, y + 7, W - 76, 17, text, kText, 6);
    }
    p.text_centre(40, H - 50, W - 80, 36, 18, "Up / down: choose    Left / right: button    Cross: do it    L1 / R1: page    Triangle: back", kSoftText);
}

// the launcher's install pages (local change, stage 2)
void Menu::draw_install_page(int ww, int wh, const Values &v, Picture &out) {
    constexpr float W = 1000, top = 46, row_h = 44, help_h = 60;
    const int downloads = static_cast<int>(v.install.downloads.size());
    const float H = top + 90 + 130 + 30 + downloads * 30 + 40 + 2 * (row_h + 8) + 16 + help_h + 54;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    title_tab(p, W, top, "Install");
    body(p, W, H, top);
    join_title_tab(p, W, top, "Install");
    p.rrect(24, top + 24, W - 48, 40, 6, kStrip);
    p.text_in(40, top + 24, 40, W - 80, 21, "The game is built on this PC from your own disc: no game data is downloaded", kText, true);
    float y = top + 90;
    p.rrect(24, y, W - 48, 118, 8, kTile, 1.5f);
    p.text_in(44, y + 8, 30, 200, 20, "Disc image", kText, true);
    p.text_in(44, y + 40, 30, W - 88, 18, v.install.iso.empty() ? std::string("none chosen yet") : v.install.iso, v.install.iso.empty() ? kDisabled : kText);
    const bool good = !v.install.iso.empty() && v.install.disc_line.empty();
    p.text_in(44, y + 76, 30, W - 88, 18, good ? std::string("The Sims 2 (PAL, SLES-53718): ready to install.") : v.install.disc_line, good ? kDarkGreen : kAmber, true);
    y += 130;
    p.text_in(44, y, 30, W - 88, 19, "Downloaded for the build:", kText, true);
    y += 30;
    for (const auto &d : v.install.downloads) {
        p.text_in(64, y, 28, W - 300, 17, d.first, kText);
        p.text_right(W - 44, y, 28, 17, d.second, kSoftText);
        y += 30;
    }
    p.text_in(44, y + 4, 30, W - 88, 17, v.install.total + (v.install.free.empty() ? "" : "   " + v.install.free), kSoftText);
    y += 40;
    const char *labels[2] = {"Choose disc image", "Install"};
    std::string help = "Cross: choose";
    for (int r = 0; r < 2; ++r) {
        const bool lit = key_sel_ == r, off = r == 1 && !v.install.can_install;
        p.rrect(24, y, W - 48, row_h, 10, off ? kTileOff : lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : off ? kDisabled : kBorder);
        p.text_centre(24, y, W - 48, row_h, 22, labels[r], off ? kDisabled : kText, true);
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, r);
        if (lit) help = r == 0 ? "Cross: look for your disc image (an .iso file) on this PC." : "Cross: download the tools and build the game. It takes a few minutes.";
        y += row_h + 8;
    }
    y += 8;
    p.rrect(24, y, W - 48, help_h, 8, kHelp);
    p.paragraph(38, y + 7, W - 76, 18, status_.empty() ? help : status_, status_.empty() ? kText : kAmber, 2);
    p.text_centre(40, H - 50, W - 80, 36, 18, "Up / down: choose    Cross: do it    Triangle / Esc: close", kSoftText);
}

void Menu::draw_picker_page(int ww, int wh, const Values &v, Picture &out) {
    constexpr float W = 1000, top = 46, row_h = 36, row_gap = 4, help_h = 44;
    constexpr int kShown = 12;
    const float H = top + 90 + kShown * (row_h + row_gap) + 30 + help_h + 54;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    const char *const title = v.picker.zips ? "Mod zip" : "Disc image";  // local change: the Mods page's "Install from zip" too
    title_tab(p, W, top, title);
    body(p, W, H, top);
    join_title_tab(p, W, top, title);
    p.rrect(24, top + 24, W - 48, 40, 6, kStrip);
    p.text_in(40, top + 24, 40, W - 80, 19, v.picker.folder.empty() ? std::string("Drives") : v.picker.folder, kText, true);
    float y = top + 90;
    const int n = static_cast<int>(v.picker.entries.size());
    if (n == 0) p.text_in(44, y, row_h, W - 88, 19, v.picker.zips ? "No zips or folders here." : "No disc images or folders here.", kDisabled);
    for (int i = key_top_; i < std::min(n, key_top_ + kShown); ++i) {
        const auto &e = v.picker.entries[i];
        const bool lit = key_sel_ == i;
        p.rrect(24, y, W - 48, row_h, 8, lit ? kSelect : kTile, lit ? 2.5f : 1.5f, lit ? kDarkGreen : kBorder);
        p.text_in(44, y, row_h, W - 220, 19, e.second == 0 ? std::string("..  (a folder up)") : e.first, kText, e.second == 2);
        p.text_right(W - 44, y, row_h, 15, e.second == 2 ? (v.picker.zips ? "zip" : "disc image") : e.second == 1 ? "folder" : "", kSoftText);
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, i);
        y += row_h + row_gap;
    }
    y = top + 90 + kShown * (row_h + row_gap);
    if (n > kShown) p.text_centre(24, y, W - 48, 24, 15, std::to_string(key_top_ + 1) + " to " + std::to_string(std::min(n, key_top_ + kShown)) + " of " + std::to_string(n), kSoftText);
    y += 30;
    p.rrect(24, y, W - 48, help_h, 8, kHelp);
    p.paragraph(38, y + 7, W - 76, 18, v.picker.zips ? "Cross: open a folder or choose a mod's .zip (it is unpacked into the mods folder)." : "Cross: open a folder or choose a disc image (.iso).", kText, 1);
    p.text_centre(40, H - 50, W - 80, 36, 18, "Up / down: choose    Cross: open / choose    L1 / R1: page    Triangle: a folder up", kSoftText);
}

void Menu::draw_progress_page(int ww, int wh, const Values &v, Picture &out) {
    constexpr float W = 1000, top = 46, row_h = 34, help_h = 64;
    const int steps = static_cast<int>(v.progress.steps.size());
    const float H = top + 90 + steps * (row_h + 4) + 20 + 30 + 20 + 44 + 16 + help_h + 54;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    const char *title = v.progress.done ? "Installed" : "Installing";
    title_tab(p, W, top, title);
    body(p, W, H, top);
    join_title_tab(p, W, top, title);
    p.rrect(24, top + 24, W - 48, 40, 6, kStrip);
    p.text_in(40, top + 24, 40, W - 80, 20, v.progress.done ? "Installed. Press Play." : "Please wait: the game is being built from your disc", kText, true);
    float y = top + 90;
    static const char *const kState[4] = {"waiting", "running", "done", "failed"};
    for (const auto &st : v.progress.steps) {
        const int state = std::clamp(st.second, 0, 3);
        p.rrect(24, y, W - 48, row_h, 8, state == 1 ? kSelect : kTile, 1.5f, kBorder);
        p.text_in(44, y, row_h, W - 260, 18, st.first, state == 0 ? kDisabled : kText, state == 1);
        p.text_right(W - 44, y, row_h, 16, kState[state], state == 3 ? kAmber : state == 2 ? kDarkGreen : kSoftText);
        y += row_h + 4;
    }
    y += 20;
    p.rrect(24, y, W - 48, 22, 11, kBox, 1.5f);
    const float f = static_cast<float>(std::clamp(v.progress.fraction, 0.0, 1.0));
    if (f > 0) p.rrect(24, y, std::max(22.0f, (W - 48) * f), 22, 11, kGreen);
    y += 30;
    p.text_in(44, y, 20, W - 88, 15, v.progress.line, kSoftText);
    y += 20;
    const char *button = !v.progress.error.empty() ? "Try again" : v.progress.done ? "Play" : "Stop";
    p.rrect(24, y, W - 48, 44, 10, kSelect, 2.5f, kDarkGreen);
    p.text_centre(24, y, W - 48, 44, 22, button, kText, true);
    add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(44), -1, -1, -1, 0);
    y += 44 + 16;
    p.rrect(24, y, W - 48, help_h, 8, kHelp);
    if (!v.progress.error.empty()) p.paragraph(38, y + 7, W - 76, 18, "It stopped: " + v.progress.error, kAmber, 2);
    else p.paragraph(38, y + 7, W - 76, 18, v.progress.done ? "Cross: start the game." : "You can stop at any time: the install goes on from where it stopped.", kText, 2);
    p.text_centre(40, H - 50, W - 80, 36, 18, "Cross: " + std::string(button) + "    Triangle / Esc: " + (!v.progress.error.empty() ? "close" : v.progress.done ? "" : "stop"), kSoftText);
}

// the on-screen keyboard for a slot's name, over the save states page (in that page's panel units)
void Menu::draw_keyboard(Picture &out) {
    const float k = static_cast<float>(out.width) / std::max(1.0f, panel_w_);
    const float W = static_cast<float>(out.width) / k, H = static_cast<float>(out.height) / k;
    Painter p{menudraw::Canvas(out), menu_font(), k};
    for (std::uint32_t &px : out.rgba) {
        if (!(px >> 24)) continue;
        const std::uint32_t r = (px & 0xFF) * 3 / 5, g = (px >> 8 & 0xFF) * 3 / 5, b = (px >> 16 & 0xFF) * 3 / 5;
        px = (px & 0xFF000000u) | b << 16 | g << 8 | r;
    }
    constexpr float key = 64, gap = 8;
    const float dw = kKbCols * key + (kKbCols - 1) * gap + 48, dh = 120 + kKbRows * (key + gap) + 70;
    const float dx = (W - dw) / 2, dy = std::max(10.0f, (H - dh) / 2);
    p.rrect(dx, dy, dw, dh, 18, hex(0x8FA0E0, 250), 3);
    p.text_in(dx + 24, dy + 12, 36, dw - 48, 22, edit_card_ >= 0 ? std::string("Name for the memory card") : "Name for slot " + std::to_string(edit_slot_), kText, true);
    // the name with a cursor
    p.rrect(dx + 24, dy + 54, dw - 48, 50, 10, kBox, 2.5f, kDarkGreen);
    const std::string shown = menufont::fit_text(p.f, edit_text_ + "|", p.S(24), p.S(dw - 80));
    p.text_in(dx + 40, dy + 54, 50, dw - 80, 24, shown, kText);
    const float gy = dy + 120;
    for (int r = 0; r < kKbRows; ++r) {
        for (int c = 0; c < kKbCols; ++c) {
            float kx = dx + 24 + c * (key + gap), kw = key;
            std::string label;
            if (r < 4) {
                char ch = kKbKeys[r][c];
                if (kb_caps_ && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
                label = std::string(1, ch);
            } else {  // the wide keys: draw each once, at its first column
                const int b = bottom_key(c);
                const int first = b == 0 ? 0 : b == 1 ? 2 : b == 2 ? 6 : 8, span = b == 0 ? 2 : b == 1 ? 4 : 2;
                if (c != first) continue;
                kw = span * key + (span - 1) * gap;
                label = b == 0 ? (kb_caps_ ? "ABC" : "abc") : b == 1 ? "Space" : b == 2 ? "Delete" : "Done";
            }
            const bool lit = r == kb_row_ && (r < 4 ? c == kb_col_ : bottom_key(c) == bottom_key(kb_col_));
            const float ky = gy + r * (key + gap);
            p.rrect(kx, ky, kw, key, 10, lit ? kSelect : kTile, lit ? 3 : 1.5f, lit ? kDarkGreen : kBorder);
            p.text_centre(kx, ky, kw, key, r < 4 ? 26 : 20, label, kText, lit);
            Hit hit;
            hit.box = {out.x + static_cast<int>(p.S(kx)), out.y + static_cast<int>(p.S(ky)), static_cast<int>(p.S(kw)), static_cast<int>(p.S(key))};
            hit.dialog = 100 + r * kKbCols + c;
            hits_.insert(hits_.begin(), hit);
        }
    }
    p.text_centre(dx + 10, dy + dh - 52, dw - 20, 36, 17, "Cross: type    L1: delete    R1: capitals    Triangle: cancel    (or type on the keyboard, Enter: done)",
                  kSoftText);
}

// the question, drawn over the page that is shown (in that page's panel units)
void Menu::draw_dialog(Picture &out) {
    const float k = static_cast<float>(out.width) / std::max(1.0f, panel_w_);
    const float W = static_cast<float>(out.width) / k, H = static_cast<float>(out.height) / k;
    Painter p{menudraw::Canvas(out), menu_font(), k};
    for (std::uint32_t &px : out.rgba) {  // the page darker under the question (only its own pixels: the corners stay clear)
        if (!(px >> 24)) continue;
        const std::uint32_t r = (px & 0xFF) * 3 / 5, g = (px >> 8 & 0xFF) * 3 / 5, b = (px >> 16 & 0xFF) * 3 / 5;
        px = (px & 0xFF000000u) | b << 16 | g << 8 | r;
    }
    const float dw = std::min(W - 40, 620.0f), dh = 230, dx = (W - dw) / 2, dy = std::max(10.0f, (H - dh) / 2);
    p.rrect(dx, dy, dw, dh, 18, hex(0x8FA0E0, 250), 3);
    p.rrect(dx + 16, dy + 16, dw - 32, 112, 10, kHelp);
    p.paragraph(dx + 30, dy + 26, dw - 60, 21, dialog_.text, kText, 4);
    const float bw = (dw - 32 - 20) / 2, by = dy + 148, bh = 56;
    for (int b = 0; b < 2; ++b) {
        const float bx = dx + 16 + b * (bw + 20);
        const bool lit = dialog_.sel == b;
        p.rrect(bx, by, bw, bh, 12, lit ? kSelect : kTile, lit ? 3 : 2, lit ? kDarkGreen : kBorder);
        p.text_centre(bx, by, bw, bh, 24, b == 0 ? "Yes" : "No", kText, true);
        Hit hit;
        hit.box = {out.x + static_cast<int>(p.S(bx)), out.y + static_cast<int>(p.S(by)), static_cast<int>(p.S(bw)), static_cast<int>(p.S(bh))};
        hit.dialog = b;
        hits_.insert(hits_.begin(), hit);  // (before the page's own boxes: the question takes the click)
    }
}

void Menu::draw_keys_page(int ww, int wh, const Values &v, Picture &out) {
    const bool pad = page_ == Page::pad;
    key_rows_ = static_cast<int>(v.key_rows.size());
    const int shown_rows = std::min(key_rows_, 11);
    keys_shown_ = std::max(1, shown_rows);
    fit_keys();
    constexpr float W = 1000, top = 46, row_h = 34, row_gap = 4;
    const float rows_y = top + 90, H = rows_y + shown_rows * (row_h + row_gap) + 10 + row_h + 24 + 30 + 28 + 28 + 26;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    Painter p{menudraw::Canvas(out), menu_font(), k};
    const std::string title = pad ? "Controller buttons" : "Change keys";
    title_tab(p, W, top, title);
    body(p, W, H, top);
    join_title_tab(p, W, top, title);
    p.rrect(24, top + 24, W - 48, 40, 6, kStrip);
    p.text_in(40, top + 24, 40, W - 300, 22, pad ? "Which controller button does what - the game waits" : "Keys and mouse buttons - the game waits", kText, true);
    if (key_rows_ > shown_rows)
        p.text_right(W - 40, top + 24, 40, 18, std::to_string(key_top_ + 1) + "-" + std::to_string(key_top_ + shown_rows) + " of " + std::to_string(key_rows_), kSoftText);
    const std::string capture = pad ? "Press a button on the controller...  (Esc: cancel)" : "Press a key or mouse button...  (Esc: cancel)";
    const auto row = [&](float y, int index, const std::string &left, const std::string &right) {
        const bool lit = index == key_sel_;
        p.rrect(24, y, W - 48, row_h, 7, lit ? kSelect : kTile, 1.5f);
        if (lit && capturing_ && index < key_rows_) {
            p.text_in(40, y, row_h, W - 80, 20, capture, kDarkGreen, true);
        } else {
            p.text_in(40, y, row_h, W * 0.38f - 40, 20, left, kText, lit);
            if (!right.empty()) p.text_in(W * 0.42f, y, row_h, W * 0.58f - 40, 20, right, kText);
        }
        add_hit(out, p.S(24), p.S(y), p.S(W - 48), p.S(row_h), -1, -1, -1, index);
    };
    float y = rows_y;
    for (int r = 0; r < shown_rows; ++r) {
        const int index = key_top_ + r;
        row(y, index, v.key_rows[index].label, v.key_rows[index].keys);
        y += row_h + row_gap;
    }
    y += 10;
    const std::string defaults = pad ? (defaults_armed_ ? "Really put back the default buttons?  Cross again" : "Put back the default buttons")
                                     : (defaults_armed_ ? "Really put back the default keys?  Cross again" : "Put back the default keys");
    row(y, key_rows_, defaults, "");
    y += row_h + 24;
    p.text_in(32, y, 30, W - 64, 20, v.key_note, kText, true);
    y += 30;
    p.text_in(32, y, 28, W - 64, 18, pad ? "Cross: change a button      Triangle: back" : "Enter / click: change a key      Esc / Triangle: back", kSoftText);
    y += 28;
    p.text_in(32, y, 28, W - 64, 18, pad ? "Select, Start, the triggers and the sticks stay as they are" : "Not usable: F1 F2 F5 F8 F9 F11 (the program's)", kSoftText);
}

void Menu::draw_note(int window_width, int window_height, const std::string &text, Picture &out) {
    const int ww = std::max(1, window_width), wh = std::max(1, window_height);
    const menufont::Font &f = menu_font();
    const float k0 = std::max(0.6f, static_cast<float>(wh) / 900.0f);
    const float tw0 = f.width(text, 24 * k0) / k0;
    const float W = tw0 + 48, H = 50;
    const float k = panel_scale(ww, wh, W, H);
    place(out, ww, wh, W, H, k);
    out.y = std::min(std::max(0, wh / 20), wh - out.height);
    Painter p{menudraw::Canvas(out), f, k};
    p.rrect(1.5f, 1.5f, W - 3, H - 3, (H - 3) / 2, hex(0x8FA0E0, 235), 2.5f);
    p.text_centre(0, 0, W, H, 24, text, kText, true);
}

}  // namespace quickmenu
