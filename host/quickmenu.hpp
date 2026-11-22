#pragma once

// The quick menu (F1, L3 + R3 or Guide on a controller, Ctrl + Alt + 0): a panel drawn over the game picture in the window, while the
// game waits. This file only knows the pages, the controls, the focus and how the panel looks; host/window.cpp opens it, holds the game,
// feeds it the keys and does what an Action asks for. The panel is a picture made here on the CPU (shapes: menu_draw.hpp, text:
// menu_font.hpp), which the window lays over the game's picture: the WebGPU present pass (runtime/src/gs/gpu/overlay.hpp) or the SDL
// renderer. The game itself, its saved pictures and its save states never see it.
//
// Local change (the remaster): a main page (Resume, Quick save, Quick load, Settings, Cheats, Debug, Quit) and a Settings page with
// three tabs (Graphics, Game, Controls) of checkboxes, round buttons, list boxes and sliders, in the look of the game's own menus.

#include "menu_draw.hpp"
#include "menu_font.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quickmenu {

using Picture = menudraw::Picture;

enum class Command {
    none, resume, save, load, look, scale, sharp_2d, fullscreen, keys, quit,
    change_keys,   // opens the keys page (the menu switches page by itself)
    key_edit,      // keys page: the focused line waits for a key (capturing() is true from now on)
    key_defaults,  // keys page: put back the default keys (asked once, like Quit)
    // the remaster options, kept in remaster.ini (host/window.cpp)
    opt_frame_rate, opt_ui_width, opt_load_turbo, opt_disc_speed, opt_pad_merge, opt_deband, opt_textures,
    change_pad,    // opens the controller buttons page
    look_filter, opt_aa,  // the picture filter (crisp / soft / sharp) and anti-aliasing (remaster.ini PS2_AA; was "smooth edges")
    open_settings, open_cheats, open_debug,
    restart,       // main page: start the game again (asked once, like Quit)
    apply,         // settings page: the changes chosen so far are to be used (the window takes them with changes())
    preset_vanilla, preset_updated, defaults,  // settings page: fill in the choices (nothing is used before Apply)
    opt_player1, opt_player2,  // which controller plays player 1 / player 2 (choices: the connected controllers)
    opt_vsync, opt_skip_intro, // wait for the screen's refresh; skip the logo movies at the start (both at the next start)
    list_toggle, list_step,    // the cheats / debug pages: a row's checkbox (value = row), a number one step (value = row, step = -1 / 1)
    list_reset_all,            // the cheats / debug pages: everything back to the game's own values (asked once)
    opt_aspect, opt_screen_res,  // the picture's shape (4:3, 16:9, 21:9; next start) and the screen resolution (used at once)
    open_states, state_save, state_load,  // the save states page; save into / load slot `value` (1 to 10)
    slot_rename,               // the save states page: slot `value` gets the name in edit_text() ("" takes the name away)
    discard_changes,           // (the menu's own: the settings' picks not applied are thrown away, back to the main page)
    opt_webcam,                // local change: which webcam is the game's EyeToy camera, or none (next start)
    // local change: the memory cards page (the card library, host/card_library.hpp). value = a card's row in Values::cards, step = slot 1 / 2
    open_cards, card_new, card_insert, card_eject /* value = slot 1 / 2 */, card_rename /* name in edit_text() */, card_copy, card_delete,
    play, open_mods,           // local change: the launcher's main page (Mode::launcher): start the game; the Mods page (stage 3)
    // local change: the launcher's install (stage 2): start / stop / try again; open the disc image picker; a picker entry (value = its
    // row, -1 = a folder up / leave); install the newer release
    install_start, install_cancel, install_retry, pick_iso, picker_open, update_now,
    // local change: the launcher's Mods page (stage 3). value = a mod's row in Values::mods.rows
    mod_zip, mod_toggle, mod_up, mod_down, mod_delete,
    // local change: the launcher's main page: download the HD textures (part of the game; host/hd_pack.hpp), stop that download
    get_hd, hd_cancel,
    opt_mouse_mode,  // local change: the mouse PC style / as a controller (remaster.ini PS2_MOUSE_MODE; used at once)
    count_
};

enum class Kind { button, checkbox, radio, list, slider };

// What one setting shows; the window fills it in before each drawing and each press.
struct Setting {
    int index = 0;                     // the chosen one of `choices` (for the next start, where that differs)
    std::vector<std::string> choices;
    bool next_start = false;           // the chosen value is used when the game starts the next time (shows "needs restart")
    bool locked = false;               // set by an environment variable; choosing only shows a note
    std::string locked_note;           // "set by PS2_SCALE"
    bool restart = false;              // a change of this setting is used only after a restart
    int vanilla = -1, updated = -1;    // the choice of the two presets (-1: the preset leaves it alone)
    int fallback = -1;                 // the program's default choice ("Defaults", -1: none)
};

// a row of the cheats and debug pages (the window fills them in)
struct ListRow {
    std::string label, value, help;  // value: a number's text
    bool header = false;             // a heading, never focused
    bool checkbox = false, on = false;
    bool number = false;             // left / right step it
    bool action = false;             // Cross does something once (value: the word on its button)
    bool changed = false;            // set from the menu (kept in remaster.ini): shows a small tag
};

struct Values {
    Setting settings[static_cast<int>(Command::count_)];
    // the save states page: slots 1 to 10 (index 0 = slot 1)
    struct Slot {
        bool used = false;
        std::string when;  // "16 Nov, 21:05"
        std::string name;  // the player's name for it, or ""
    };
    Slot slots[10];
    // the memory cards page: every card of the library, and which one is in each slot ("" = none)
    struct Card {
        std::string name;
        std::string when;  // when it was last written ("" = never: a new card)
        int slot = 0;      // 0 in no slot, 1 or 2
    };
    std::vector<Card> cards;
    std::string slot_cards[2];
    bool card_library = false;  // false: the run has no card library (--card-library), the page only says so
    // local change: the launcher's install pages (stage 2)
    struct Install {
        std::string iso;          // the chosen disc image ("" = none yet)
        std::string disc_line;    // what is wrong with it ("" = it is the right disc)
        bool can_install = false;
        std::vector<std::pair<std::string, std::string>> downloads;  // what is downloaded, and how big
        std::string total, free;  // "about 330 MB to download, 3 GB of space needed", "512 GB free on C:"
    } install;
    struct Picker {
        std::string folder;                             // "" = the list of drives
        std::vector<std::pair<std::string, int>> entries;  // name, kind: 0 a folder up, 1 a folder or drive, 2 a disc image (a zip)
        bool zips = false;                              // local change: choosing a mod's .zip instead of a disc image
    } picker;
    // local change: the launcher's Mods page (stage 3), highest priority first
    struct ModRow {
        std::string name, info;          // info: "by X, version Y"
        std::string details;             // description, what it has, what is ignored
        std::vector<std::string> warnings;
        bool on = false, can_on = true;
    };
    struct Mods {
        std::vector<ModRow> rows;
    } mods;
    struct Progress {
        std::vector<std::pair<std::string, int>> steps;  // name, state: 0 waiting, 1 running, 2 done, 3 failed
        double fraction = 0;
        std::string line;   // the newest line of the build
        std::string error;  // "" = no error
        bool done = false;
    } progress;
    std::vector<ListRow> list_rows;
    // the keys page: one row per button (label and its keys, in the order of keys.ini) and a message line (what the last change did)
    struct KeyRow {
        std::string label, keys;
    };
    std::vector<KeyRow> key_rows;
    std::string key_note;
};

// What the window should do: a command, and for a setting the index of the chosen choice.
struct Action {
    Command cmd = Command::none;
    int value = 0;
    int step = 0;  // list_step: -1 or 1
};

enum class Input { up, down, left, right, confirm, back, tab_prev, tab_next };
enum class Page { main, settings, keys, pad, cheats, debug, states, cards, install, pick_iso, progress, mods };
// game: the quick menu over the running game; launcher: Sims2Launcher.exe's menu (Play, Memory cards, Mods, Settings, Quit)
enum class Mode { game, launcher };

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
};

// The settings page has two levels. On the first the pad moves over the categories (the framed groups, two columns side by side, and
// the row of buttons below them: Vanilla, Updated, Defaults, Apply); Cross opens a category. Inside one, up / down goes from setting
// to setting, left / right moves the highlight over a setting's choices and Cross picks the highlighted one. Nothing is used until
// Apply: picked choices are kept here (pending) and shown with a "not applied" tag.
constexpr int kActionCount = 4;  // the buttons below the categories

class Menu {
public:
    explicit Menu(Mode mode = Mode::game);
    // the launcher: show one of its pages (install, pick_iso, progress, main) and say whether a newer release exists ("" = none)
    void show(Page page);
    void set_update(const std::string &version) { update_ = version; }
    // the launcher: the HD textures are missing - a tile at the top ("" = they are there); busy: they are being downloaded (the tile's
    // text is the progress, Cross asks to stop). A newer release (set_update) takes the tile's place: the update brings them too.
    void set_hd(const std::string &label, bool busy) { hd_label_ = label, hd_busy_ = busy; }
    Mode mode() const { return mode_; }
    bool is_open() const { return open_; }
    void open();
    void close() { open_ = false; }
    Page page() const { return page_; }
    int tab() const { return tab_; }
    // main page: the focused entry; keys pages: the line; settings: the focused control inside a category (-1 on the first level)
    int focus() const {
        return page_ == Page::keys || page_ == Page::pad || list_page() || page_ == Page::states || page_ == Page::cards || page_ == Page::mods ? key_sel_ : page_ == Page::main ? focus_ : inside_ ? row_ : -1;
    }
    // the memory cards page: rows 0 / 1 are the slots, 2 "New card", then the cards; on a card's row the column is
    // 0 Slot 1, 1 Slot 2, 2 Rename, 3 Copy, 4 Delete
    int card_column() const { return card_col_; }
    static constexpr int kCardFirstRow = 3, kCardColumns = 5;
    // the Mods page (launcher): row 0 "Install from zip", then the mods; on a mod's row the column is 0 On / Off, 1 Up, 2 Down,
    // 3 Delete (card_column())
    static constexpr int kModFirstRow = 1, kModColumns = 4;
    bool list_page() const { return page_ == Page::cheats || page_ == Page::debug; }
    bool inside() const { return inside_; }            // settings: a category is open
    int cell() const { return cell_; }                 // settings, first level: a category (0..) or, after them, one of the buttons
    int category_count() const;                        // the categories of the tab shown
    int highlight() const { return hl_; }              // the highlighted choice of the focused setting
    bool list_open() const { return list_open_; }
    int list_highlight() const { return list_hl_; }
    // a question over the page (Yes / No): apply, restart, quit, load, save over a slot, put back defaults, throw away changes
    bool dialog_open() const { return dialog_.open; }
    int dialog_choice() const { return dialog_.sel; }  // 0 Yes, 1 No
    const std::string &dialog_text() const { return dialog_.text; }
    int load_slot() const { return load_slot_; }       // the save states page: the slot its Load row shows
    int slot_column() const { return slot_col_; }      // the save states page: 0 Save, 1 Rename on a slot's row
    // naming a slot: an on-screen keyboard over the save states page (the pad), or the computer's keyboard (type_text / erase_text)
    bool editing() const { return editing_; }
    const std::string &edit_text() const { return edit_text_; }
    void type_text(const std::string &utf8);  // typed on the computer's keyboard
    void erase_text();                        // Backspace
    Action finish_edit(bool keep);            // Enter / Esc
    bool quit_armed() const { return armed_ == Command::quit; }
    bool restart_armed() const { return armed_ == Command::restart; }
    bool discard_armed() const { return discard_armed_; }
    // the choices picked and not applied yet (pending), and what Apply hands to the window
    int pending(Command c) const { return pending_[static_cast<int>(c)]; }
    int change_count() const;
    std::vector<Action> changes() const;
    void clear_changes();
    // a line shown in the description box until the next press ("3 changes applied")
    void set_status(const std::string &text) { status_ = text; }
    // a line under the main page's tiles until it is set again ("" = none): the launcher's card slots, or why Play did not start
    void set_main_status(const std::string &text) { main_status_ = text; }
    // a key, button or stick push; back on the main page answers Command::resume (the window closes the menu)
    Action press(Input in, const Values &v);
    // the mouse, in window pixels, on the boxes of the last drawing: a click does what the pad would; hover only moves the focus
    Action click(int x, int y, const Values &v);
    bool hover(int x, int y, const Values &v);
    // the keys pages
    bool on_keys_page() const { return page_ == Page::keys; }
    bool on_pad_page() const { return page_ == Page::pad; }
    bool capturing() const { return capturing_; }   // a line waits for the next key or button
    int key_row() const { return key_sel_; }
    int key_top() const { return key_top_; }        // the first line the keys page shows
    void stop_capture() { capturing_ = false; }
    void set_key_rows(int count) { key_rows_ = count; }
    void draw(int window_width, int window_height, const Values &values, Picture &out);
    // the width of the page drawn last in panel units (window pixels per unit = picture width / this); its title tab is the top
    // kTitleUnits units (the launcher drags its borderless window by it)
    float panel_units() const { return panel_w_; }
    static constexpr float kTitleUnits = 46;
    // A small panel with one line of text at the top of the window (for a few seconds after the menu is closed: "Saved", "Loaded")
    static void draw_note(int window_width, int window_height, const std::string &text, Picture &out);
    // where the last drawing put a control's choice (choice -1: the whole control), in window pixels; for tests
    Box hit_box(Command c, int choice) const;
    // local change: where the last drawing put the Back button, in window pixels (w = 0: none); the launcher's borderless window must
    // not treat a press there as dragging the window by its title row
    Box back_box() const;

private:
    struct Hit {
        Box box;
        int control = -1;  // index on the current page/tab, or -1
        int choice = -1;   // a radio choice, a list row or a slider step; -1 the control itself
        int tab = -1;      // a tab of the settings page
        int key_row = -1;  // a line of the keys pages
        int cell = -1;     // settings: a category's frame or one of the buttons below them
        int dialog = -1;   // the question's Yes (0) and No (1)
        bool back = false; // local change: the Back button at the top left (every page but the main page and the install pages)
    };
    struct Dialog {
        bool open = false;
        std::string text;
        Action yes;
        int sel = 1;
    };
    Dialog dialog_;
    int load_slot_ = 1;
    int slot_col_ = 0;
    bool editing_ = false, kb_caps_ = true;
    int edit_slot_ = 0, kb_row_ = 1, kb_col_ = 0;
    int edit_card_ = -1;  // naming a card (its row in Values::cards) instead of a slot
    int card_col_ = 0, cards_shown_ = 7;
    std::string edit_text_;
    float panel_w_ = 1;  // the width of the page drawn last, in panel units (for the question drawn over it)
    bool open_ = false;
    Mode mode_ = Mode::game;
    std::string main_status_;
    std::string update_;
    std::string hd_label_;
    bool hd_busy_ = false;
    // the main page's entries for this mode
    int main_count() const;
    bool main_disabled(int i) const;
    Page page_ = Page::main;
    int tab_ = 0, focus_ = 0, main_return_ = 3, keys_return_ = 0;
    bool inside_ = false;  // settings: a category is open
    int cell_ = 0, row_ = 0, hl_ = 0;
    bool list_open_ = false;
    int list_hl_ = 0, list_top_ = 0;
    Command armed_ = Command::none;  // main page: Quit or Restart asked once
    bool discard_armed_ = false;     // settings: back with changes not applied asked once
    std::string status_;
    int pending_[static_cast<int>(Command::count_)];
    int key_rows_ = 0, key_sel_ = 0, key_top_ = 0, keys_shown_ = 11;  // (11: the most lines the keys page shows at once)
    bool capturing_ = false, defaults_armed_ = false;
    std::vector<Hit> hits_;
    void draw_main(int window_width, int window_height, Picture &out);
    void draw_settings(int window_width, int window_height, const Values &values, Picture &out);
    void draw_keys_page(int window_width, int window_height, const Values &values, Picture &out);
    void add_hit(const Picture &out, float x, float y, float w, float h, int control, int choice, int tab, int key_row, int cell = -1);
    Action press_keys(Input in);
    Action press_list_page(Input in, const Values &v);
    Action press_states(Input in, const Values &v);
    Action press_cards(Input in, const Values &v);
    Action press_mods(Input in, const Values &v);  // local change (launcher stage 3)
    void draw_mods_page(int window_width, int window_height, const Values &values, Picture &out);
    int mods_shown_ = 6;
    Action press_install(Input in, const Values &v);
    Action press_picker(Input in, const Values &v);
    Action press_progress(Input in, const Values &v);
    void draw_install_page(int window_width, int window_height, const Values &values, Picture &out);
    void draw_picker_page(int window_width, int window_height, const Values &values, Picture &out);
    void draw_progress_page(int window_width, int window_height, const Values &values, Picture &out);
    void draw_cards_page(int window_width, int window_height, const Values &values, Picture &out);
    Action press_dialog(Input in);
    Action press_keyboard(Input in);
    void draw_keyboard(Picture &out);
    void ask(const std::string &text, Action yes, bool default_yes);
    void draw_states_page(int window_width, int window_height, const Values &values, Picture &out);
    void draw_dialog(Picture &out);
    void draw_back_button(Picture &out);  // local change
    Action back_click(const Values &v);
    void draw_list_page(int window_width, int window_height, const Values &values, Picture &out);
    void fit_list(const Values &v);
    Action press_list(Input in, const Values &v);
    Action press_main(Input in);
    Action press_settings(Input in, const Values &v);
    Action press_grid(Input in, const Values &v);
    int first_button() const;  // local change: the first button shown below the categories (Vanilla / Updated: Graphics tab only)
    Action press_inside(Input in, const Values &v);
    Action use(int control, Input in, const Values &v);
    Action do_button(int button, const Values &v);
    void enter(int control, const Values &v);
    int shown(Command c, const Values &v) const;
    void pick(Command c, int choice, const Values &v);
    void switch_tab(int by);
    void fit_keys();
};

}  // namespace quickmenu
