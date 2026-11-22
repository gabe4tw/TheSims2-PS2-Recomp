// The quick menu's pages, tabs and controls (host/quickmenu.hpp), no game and no window needed: ninja ps2menutest && ./ps2menutest
#include "../host/quickmenu.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failed = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

using quickmenu::Action;
using quickmenu::Command;
using quickmenu::Input;
using quickmenu::Menu;
using quickmenu::Page;

quickmenu::Setting &at(quickmenu::Values &v, Command c) { return v.settings[static_cast<int>(c)]; }

quickmenu::Values values() {
    quickmenu::Values v;
    at(v, Command::look_filter) = {3, {"Off", "Crisp", "Soft", "Sharp"}};
    at(v, Command::opt_aa) = {1, {"Off", "Fast", "2x", "3x", "4x", "5x", "6x", "7x", "8x"}};
    at(v, Command::scale) = {2, {"1x", "2x", "3x", "4x"}};
    at(v, Command::opt_deband) = {0, {"off", "light", "strong"}};
    at(v, Command::opt_textures) = {1, {"original", "updated (HD)"}};
    at(v, Command::opt_ui_width) = {0, {"75", "80", "85", "90", "100"}};
    at(v, Command::sharp_2d) = {1, {"Soft", "Sharp", "Sharpest"}};
    at(v, Command::fullscreen) = {1, {"Windowed", "Borderless", "Full screen"}};
    at(v, Command::opt_frame_rate) = {1, {"50", "60", "75", "90", "100", "120", "144", "165", "240"}};
    at(v, Command::opt_load_turbo) = {1, {"off", "on"}};
    at(v, Command::opt_disc_speed) = {0, {"1x", "2x", "4x", "8x", "16x"}};
    at(v, Command::opt_player1) = {1, {"Automatic", "All controllers", "1: Xbox 360 Controller", "2: Xbox 360 Controller"}};
    at(v, Command::opt_player2) = {0, {"Automatic", "Nobody", "1: Xbox 360 Controller", "2: Xbox 360 Controller"}};
    return v;
}

bool is(const Action &a, Command c, int value = -1) { return a.cmd == c && (value < 0 || a.value == value); }

// opens the menu and goes to the settings page's tab `tab` (first level, first category)
void to_tab(Menu &m, const quickmenu::Values &v, int tab) {
    m.open();
    for (int i = 0; i < 5; ++i) m.press(Input::down, v);  // Resume -> Settings
    m.press(Input::confirm, v);
    for (int i = 0; i < tab; ++i) m.press(Input::tab_next, v);
}

}  // namespace

int main() {
    const quickmenu::Values v = values();
    {
        Menu m;
        m.open();
        check(m.is_open() && m.page() == Page::main && m.focus() == 0, "open_starts_on_resume");
    }
    {
        Menu m;
        m.open();
        for (int i = 0; i < 6; ++i) m.press(Input::down, v);
        const bool down_ok = m.focus() == 6;  // Cheats
        m.open();
        m.press(Input::up, v);
        check(down_ok && m.focus() == 9, "main_walk_wraps");
    }
    {
        Menu m;
        m.open();
        for (int i = 0; i < 5; ++i) m.press(Input::down, v);
        const Action a = m.press(Input::confirm, v);
        check(is(a, Command::open_settings) && m.page() == Page::settings && m.tab() == 0 && !m.inside() && m.cell() == 0, "confirm_settings_opens_graphics");
    }
    {
        Menu m;
        to_tab(m, v, 0);
        for (int i = 0; i < 3; ++i) m.press(Input::tab_next, v);
        const bool round = m.tab() == 0;
        m.press(Input::tab_prev, v);
        check(round && m.tab() == 2, "tabs_wrap");
    }
    {
        // Graphics: Picture on the left, Interface and Display on the right, then the four buttons
        Menu m;
        to_tab(m, v, 0);
        const int G = m.category_count();
        m.press(Input::right, v);
        const bool right = m.cell() == 1;
        m.press(Input::down, v);
        const bool down = m.cell() == 2;
        m.press(Input::left, v);
        const bool left = m.cell() == 0;
        m.press(Input::down, v);
        const bool buttons = m.cell() == G;
        for (int i = 0; i < 4; ++i) m.press(Input::right, v);
        const bool clamp = m.cell() == G + 3;
        m.press(Input::up, v);
        check(G == 3 && right && down && left && buttons && clamp && m.cell() == 2, "grid_moves_between_categories");
    }
    {
        // inside a category: left / right only highlights, Cross picks, nothing is used before Apply
        Menu m;
        quickmenu::Values w = v;
        at(w, Command::opt_deband).index = 0;
        to_tab(m, w, 0);
        m.press(Input::confirm, w);  // into Picture, on Filter
        for (int i = 0; i < 3; ++i) m.press(Input::down, w);  // Filter, Anti-aliasing and Render resolution are sliders: on to Smooth colour bands
        const bool in = m.inside() && m.focus() == 3 && m.highlight() == 0;
        const Action left = m.press(Input::left, w);
        const Action right = m.press(Input::right, w);
        const bool moved = is(left, Command::none) && is(right, Command::none) && m.highlight() == 1 && m.pending(Command::opt_deband) < 0;
        const Action pick = m.press(Input::confirm, w);
        const bool picked = is(pick, Command::none) && m.pending(Command::opt_deband) == 1 && m.change_count() == 1;
        m.press(Input::left, w);
        m.press(Input::confirm, w);  // back to the value in use: nothing to apply
        check(in && moved && picked && m.pending(Command::opt_deband) < 0 && m.change_count() == 0, "radio_highlight_then_pick");
    }
    {
        // a slider takes left / right at once (no highlight first), still waiting for Apply
        Menu m;
        to_tab(m, v, 0);
        m.press(Input::confirm, v);
        m.press(Input::down, v);
        m.press(Input::down, v);  // Resolution (3x)
        const Action a = m.press(Input::left, v);
        check(is(a, Command::none) && m.pending(Command::scale) == 1 && m.change_count() == 1, "slider_picks_at_once");
    }
    {
        Menu m;
        to_tab(m, v, 0);
        m.press(Input::confirm, v);
        m.press(Input::down, v);  // Anti-aliasing (Fast)
        m.press(Input::left, v);  // Off highlighted
        m.press(Input::confirm, v);
        const bool toggled = m.pending(Command::opt_aa) == 0;
        m.press(Input::back, v);
        const bool out = !m.inside() && m.cell() == 0;
        m.press(Input::down, v);
        for (int i = 0; i < 3; ++i) m.press(Input::right, v);  // Apply
        const Action ask = m.press(Input::confirm, v);              // "Apply 1 change?" (Yes chosen)
        const bool asked = is(ask, Command::none) && m.dialog_open() && m.dialog_choice() == 0;
        const Action a = m.press(Input::confirm, v);
        const std::vector<Action> c = m.changes();
        check(toggled && out && asked && is(a, Command::apply) && !m.dialog_open() && c.size() == 1 && is(c[0], Command::opt_aa, 0),
              "aa_radio_pick_then_apply");
    }
    {
        Menu m;
        to_tab(m, v, 1);  // Game: Speed (only Frame rate: Cross opens its list at once)
        m.press(Input::confirm, v);
        const bool opened = m.list_open() && m.list_highlight() == 1;
        m.press(Input::down, v);
        m.press(Input::down, v);
        const bool moved = m.list_highlight() == 3;
        const Action pick = m.press(Input::confirm, v);
        const bool picked = is(pick, Command::none) && m.pending(Command::opt_frame_rate) == 3 && !m.list_open();
        m.press(Input::confirm, v);
        m.press(Input::down, v);
        const Action cancel = m.press(Input::back, v);
        check(opened && moved && picked && is(cancel, Command::none) && !m.list_open() && m.page() == Page::settings, "list_open_pick_cancel");
    }
    {
        // the frame rate in use, picked again: nothing to apply (it used to rewrite remaster.ini)
        Menu m;
        to_tab(m, v, 1);
        m.press(Input::confirm, v);
        m.press(Input::confirm, v);
        m.press(Input::confirm, v);
        check(m.change_count() == 0, "same_frame_rate_no_change");
    }
    {
        Menu m;
        to_tab(m, v, 1);
        m.press(Input::confirm, v);
        m.press(Input::back, v);
        const bool out = !m.inside() && m.page() == Page::settings;
        m.press(Input::back, v);
        const bool main = m.page() == Page::main && m.focus() == 5;
        check(out && main && is(m.press(Input::back, v), Command::resume), "back_levels");
    }
    {
        // back from the settings with changes not applied asks once, then throws them away
        Menu m;
        to_tab(m, v, 0);
        m.press(Input::confirm, v);
        m.press(Input::left, v);
        m.press(Input::confirm, v);
        m.press(Input::back, v);
        m.press(Input::back, v);
        const bool asked = m.dialog_open() && m.dialog_choice() == 1 && m.page() == Page::settings;  // No chosen first
        m.press(Input::confirm, v);
        const bool kept = !m.dialog_open() && m.change_count() == 1 && m.page() == Page::settings;
        m.press(Input::back, v);
        m.press(Input::left, v);  // Yes
        m.press(Input::confirm, v);
        check(asked && kept && m.page() == Page::main && m.change_count() == 0, "discard_asks_once");
    }
    {
        Menu m;
        quickmenu::Values w = v;
        at(w, Command::look_filter).vanilla = 1;
        at(w, Command::scale).vanilla = 0;
        at(w, Command::opt_frame_rate).vanilla = 0;
        at(w, Command::opt_textures).vanilla = 0;
        at(w, Command::opt_aa).vanilla = 1;  // the value in use already
        to_tab(m, w, 0);
        const int G = m.category_count();
        m.press(Input::down, w);  // the Vanilla button
        const Action a = m.press(Input::confirm, w);
        check(is(a, Command::preset_vanilla) && m.change_count() == 4 && m.pending(Command::opt_frame_rate) == 0 && m.cell() == G + 3,
              "vanilla_fills_every_tab");
    }
    {  // local change: Vanilla / Updated are shown on the Graphics tab only; the other tabs' row is Defaults, Apply
        Menu m;
        to_tab(m, v, 1);
        const int G = m.category_count();
        m.press(Input::down, v);  // from the left column: Defaults
        const bool on_defaults = m.cell() == G + 2;
        m.press(Input::left, v);  // nothing to the left of it
        const bool stays = m.cell() == G + 2;
        m.press(Input::right, v);
        const bool apply = m.cell() == G + 3;
        m.press(Input::up, v);  // Apply is the right half: back up into the right column (or the left one when it is empty)
        const bool up = m.cell() < G;
        Menu g;
        to_tab(g, v, 0);
        g.press(Input::down, v);
        check(on_defaults && stays && apply && up && g.cell() == g.category_count(), "presets_on_graphics_tab_only");
    }
    {
        Menu m;
        quickmenu::Values w = v;
        at(w, Command::opt_frame_rate).fallback = 0;  // Game tab
        at(w, Command::scale).fallback = 0;           // Graphics tab
        to_tab(m, w, 0);
        m.press(Input::down, w);
        m.press(Input::right, w);
        m.press(Input::right, w);  // Defaults
        m.press(Input::confirm, w);
        check(m.change_count() == 1 && m.pending(Command::scale) == 0, "defaults_this_tab_only");
    }
    {
        Menu m;
        m.open();
        m.press(Input::up, v);  // Quit
        const Action first = m.press(Input::confirm, v);
        const bool asked = is(first, Command::none) && m.dialog_open() && m.dialog_choice() == 1;
        const Action no = m.press(Input::confirm, v);  // No
        m.press(Input::confirm, v);
        m.press(Input::right, v);  // Yes
        const Action yes = m.press(Input::confirm, v);
        check(asked && is(no, Command::none) && is(yes, Command::quit) && !m.dialog_open(), "quit_asks_once");
    }
    {
        Menu m;
        m.open();
        m.press(Input::up, v);
        m.press(Input::up, v);  // Restart
        const Action first = m.press(Input::confirm, v);
        const bool asked = is(first, Command::none) && m.dialog_open();
        m.press(Input::left, v);
        check(asked && is(m.press(Input::confirm, v), Command::restart), "restart_asks_once");
    }
    {
        Menu m;
        to_tab(m, v, 2);
        m.press(Input::confirm, v);  // Players: Player 1 list
        m.press(Input::confirm, v);
        m.press(Input::down, v);
        m.press(Input::confirm, v);
        check(m.pending(Command::opt_player1) == 2, "player1_list_picks_controller");
    }
    {
        Menu m;
        m.set_key_rows(5);
        to_tab(m, v, 2);
        m.press(Input::right, v);    // Keys and buttons
        m.press(Input::confirm, v);  // Show keys
        m.press(Input::down, v);     // Change keys
        const Action a = m.press(Input::confirm, v);
        const bool on_keys = is(a, Command::change_keys) && m.page() == Page::keys && m.key_row() == 0;
        for (int i = 0; i < 6; ++i) m.press(Input::down, v);
        const bool wrapped = m.key_row() == 0;
        m.press(Input::back, v);
        check(on_keys && wrapped && m.page() == Page::settings && m.tab() == 2 && m.inside() && m.focus() == 3, "keys_page_no_back_row");
    }
    {
        Menu m;
        to_tab(m, v, 1);
        m.press(Input::confirm, v);
        m.press(Input::confirm, v);
        m.close();
        m.open();
        check(m.page() == Page::main && m.focus() == 0 && !m.list_open() && !m.quit_armed() && m.change_count() == 0, "open_resets_everything");
    }
    {
        Menu m;
        quickmenu::Values w = v;
        at(w, Command::scale).locked = true;
        to_tab(m, w, 0);
        m.press(Input::confirm, w);
        m.press(Input::down, w);
        m.press(Input::down, w);  // Resolution
        m.press(Input::right, w);
        check(is(m.press(Input::confirm, w), Command::scale, 3) && m.change_count() == 0, "locked_setting_still_emits_action");
    }
    {
        // the cheats page: headings are skipped, Cross switches a checkbox, left / right step a number, the last row asks once
        quickmenu::Values w = v;
        w.list_rows = {{"Gnome cheats", "", "", true}, {"Cheat gnome", "", "help", false, true, false},
                       {"Skip to lot", "255", "help", false, false, false, true}, {"Hidden", "", "", true}, {"God mode", "", "h", false, true, true}};
        Menu m;
        m.open();
        for (int i = 0; i < 6; ++i) m.press(Input::down, w);
        const Action open = m.press(Input::confirm, w);
        const bool on_page = is(open, Command::open_cheats) && m.page() == Page::cheats;
        const Action toggle = m.press(Input::confirm, w);  // the first focusable row: Cheat gnome
        m.press(Input::down, w);                            // Skip to lot
        const Action step = m.press(Input::left, w);
        m.press(Input::down, w);                            // over the heading to God mode
        const bool skipped = m.focus() == 4;
        m.press(Input::down, w);                            // the last row
        const Action first = m.press(Input::confirm, w);
        const bool asked = m.dialog_open();
        m.press(Input::left, w);
        const Action second = m.press(Input::confirm, w);
        m.press(Input::back, w);
        check(on_page && is(toggle, Command::list_toggle, 1) && is(step, Command::list_step, 2) && step.step == -1 && skipped && asked &&
                  is(first, Command::none) && is(second, Command::list_reset_all) && m.page() == Page::main && m.focus() == 6,
              "cheats_page_rows");
    }
    {
        // save states: Load row picks among used slots with left / right and asks; an empty slot saves at once, a used one asks
        quickmenu::Values w = v;
        w.slots[2] = {true, "16 Nov, 21:05"};
        w.slots[6] = {true, "16 Nov, 21:40"};
        Menu m;
        m.open();
        for (int i = 0; i < 3; ++i) m.press(Input::down, w);
        m.press(Input::confirm, w);
        const bool on_page = m.page() == Page::states && m.focus() == 0;
        m.press(Input::right, w);
        const bool picked = m.load_slot() == 7;
        m.press(Input::confirm, w);
        const bool asks_load = m.dialog_open();
        m.press(Input::left, w);
        const Action load = m.press(Input::confirm, w);
        m.press(Input::down, w);  // slot 1, empty
        const Action save_empty = m.press(Input::confirm, w);
        m.press(Input::down, w);
        m.press(Input::down, w);  // slot 3, used
        const Action ask_over = m.press(Input::confirm, w);
        const bool asks_over = m.dialog_open() && m.dialog_choice() == 1;
        m.press(Input::back, w);  // Triangle: No
        m.press(Input::back, w);
        check(on_page && picked && asks_load && is(load, Command::state_load, 7) && is(save_empty, Command::state_save, 1) && is(ask_over, Command::none) &&
                  asks_over && m.page() == Page::main && m.focus() == 3,
              "save_states_page");
    }
    {
        // renaming a used slot: Right picks Rename, Cross opens the keyboard with the old name; keys type, L1 deletes, Done finishes
        quickmenu::Values w = v;
        w.slots[1] = {true, "16 Nov, 21:05", "Old"};
        Menu m;
        m.open();
        for (int i = 0; i < 3; ++i) m.press(Input::down, w);
        m.press(Input::confirm, w);
        m.press(Input::down, w);
        m.press(Input::right, w);  // slot 1 is empty: no Rename
        const bool empty_no_rename = m.slot_column() == 0;
        m.press(Input::down, w);   // slot 2
        m.press(Input::right, w);
        m.press(Input::confirm, w);
        const bool open = m.editing() && m.edit_text() == "Old";
        m.press(Input::tab_prev, w);  // L1: delete
        m.press(Input::tab_prev, w);
        m.press(Input::tab_prev, w);
        m.press(Input::confirm, w);   // the first key of the letter row: Q (caps: the name is empty)
        m.type_text("uiet");
        const bool typed = m.edit_text() == "Quiet";
        for (int i = 0; i < 3; ++i) m.press(Input::down, w);  // the bottom row
        m.press(Input::left, w);       // Done (going round from Caps)
        const Action done = m.press(Input::confirm, w);
        m.press(Input::right, w);
        m.press(Input::confirm, w);
        const bool reopened = m.editing();
        m.press(Input::back, w);       // Triangle: cancel
        check(empty_no_rename && open && typed && is(done, Command::slot_rename, 2) && reopened && !m.editing() && m.page() == Page::states,
              "rename_slot");
    }
    {
        // local change: the memory cards page. Rows: slot 1, slot 2, New card, then the cards with Slot 1 / Slot 2 / Rename / Copy / Delete
        quickmenu::Values w = v;
        w.card_library = true;
        w.slot_cards[0] = "Pleasantview";
        w.cards = {{"Pleasantview", "17 Nov, 01:00", 1}, {"Twinbrook", "", 0}};
        Menu m;
        m.open();
        for (int i = 0; i < 4; ++i) m.press(Input::down, w);
        const Action open = m.press(Input::confirm, w);
        const bool on_page = is(open, Command::open_cards) && m.page() == Page::cards && m.focus() == 0;
        m.press(Input::confirm, w);  // slot 1 holds a card: it asks before taking it out
        const bool asks_eject = m.dialog_open();
        m.press(Input::left, w);
        const Action eject = m.press(Input::confirm, w);
        m.press(Input::down, w);
        const Action empty_slot = m.press(Input::confirm, w);  // slot 2 is empty: only a note
        m.press(Input::down, w);
        const Action made = m.press(Input::confirm, w);  // New card
        m.press(Input::down, w);                          // Pleasantview (in slot 1)
        m.press(Input::right, w);                         // Slot 2
        const Action in_other = m.press(Input::confirm, w);  // in slot 1 already: not into slot 2
        m.press(Input::right, w);
        m.press(Input::right, w);
        m.press(Input::right, w);                         // Delete
        const Action del_in_slot = m.press(Input::confirm, w);
        const bool no_ask = !m.dialog_open();
        m.press(Input::down, w);                          // Twinbrook, the column stays Delete
        m.press(Input::confirm, w);
        const bool asks_delete = m.dialog_open();
        m.press(Input::left, w);
        const Action del = m.press(Input::confirm, w);
        for (int i = 0; i < 3; ++i) m.press(Input::left, w);  // Slot 2
        const Action insert = m.press(Input::confirm, w);
        m.press(Input::right, w);                         // Rename
        m.press(Input::confirm, w);
        const bool editing = m.editing() && m.edit_text() == "Twinbrook";
        m.type_text("x");
        const Action renamed = m.finish_edit(true);
        m.press(Input::back, w);
        check(on_page && asks_eject && is(eject, Command::card_eject, 1) && is(empty_slot, Command::none) && is(made, Command::card_new) &&
                  is(in_other, Command::none) && is(del_in_slot, Command::none) && no_ask && asks_delete && is(del, Command::card_delete, 1) &&
                  is(insert, Command::card_insert, 1) && insert.step == 2 && editing && is(renamed, Command::card_rename, 1) &&
                  m.edit_text() == "Twinbrookx" && m.page() == Page::main && m.focus() == 4,
              "memory_cards_page");
    }
    // ---- local change: the launcher's main page (Menu(Mode::launcher)) ----
    {
        Menu m(quickmenu::Mode::launcher);
        m.open();
        const bool on_play = m.focus() == 0 && m.mode() == quickmenu::Mode::launcher;
        m.press(Input::down, v);
        const bool cards = m.focus() == 1;
        m.press(Input::down, v);  // Mods (stage 3: no longer greyed)
        const bool mods = m.focus() == 2;
        m.press(Input::down, v);
        const bool settings = m.focus() == 3 && mods;
        m.press(Input::down, v);
        const bool quit = m.focus() == 4;
        m.press(Input::down, v);
        check(on_play && cards && settings && quit && m.focus() == 0, "launcher_main_order");
    }
    {
        Menu m(quickmenu::Mode::launcher);
        m.open();
        check(is(m.press(Input::confirm, v), Command::play) && !m.dialog_open(), "launcher_play_action");
    }
    {
        Menu m(quickmenu::Mode::launcher);
        m.open();
        const Action back = m.press(Input::back, v);
        const bool asked = m.dialog_open() && is(back, Command::none);
        m.press(Input::left, v);
        check(asked && is(m.press(Input::confirm, v), Command::quit), "launcher_back_asks");
        m.open();
        for (int i = 0; i < 4; ++i) m.press(Input::down, v);  // Quit
        m.press(Input::confirm, v);
        check(m.dialog_open(), "launcher_quit_asks");
    }
    {
        Menu m(quickmenu::Mode::launcher);
        m.open();
        m.press(Input::down, v);
        const Action open = m.press(Input::confirm, v);
        const bool on_cards = is(open, Command::open_cards) && m.page() == Page::cards;
        m.press(Input::back, v);
        check(on_cards && m.page() == Page::main && m.focus() == 1, "launcher_cards_and_back");
    }
    {
        Menu m(quickmenu::Mode::launcher);
        m.open();
        for (int i = 0; i < 3; ++i) m.press(Input::down, v);
        const Action open = m.press(Input::confirm, v);
        check(is(open, Command::open_settings) && m.page() == Page::settings && m.tab() == 0 && !m.inside(), "launcher_settings_reused");
        for (int i = 0; i < 2; ++i) m.press(Input::tab_next, v);
        m.press(Input::right, v);    // Keys and buttons
        m.press(Input::confirm, v);  // Show keys
        const Action keys = m.press(Input::confirm, v);
        m.press(Input::down, v);     // Change keys
        const Action change = m.press(Input::confirm, v);
        check(is(keys, Command::none) && is(change, Command::none) && m.page() == Page::settings, "launcher_key_pages_disabled");
    }
    {
        Menu m;
        m.open();
        for (int i = 0; i < 9; ++i) m.press(Input::down, v);
        const bool last = m.focus() == 9;
        m.press(Input::down, v);
        check(m.mode() == quickmenu::Mode::game && last && m.focus() == 0, "game_mode_unchanged");
    }
    // ---- local change: the launcher's install pages (stage 2) ----
    {
        quickmenu::Values w = v;
        Menu m(quickmenu::Mode::launcher);
        m.open();
        m.show(Page::install);
        w.install.can_install = false;
        w.install.disc_line = "The US version (SLUS-21265) is not supported yet.";
        m.press(Input::down, w);  // Install
        const Action refused = m.press(Input::confirm, w);
        w.install.can_install = true;
        const Action start = m.press(Input::confirm, w);
        m.press(Input::up, w);  // Choose disc image
        const Action pick = m.press(Input::confirm, w);
        check(m.page() == Page::install && is(refused, Command::none) && is(start, Command::install_start) && is(pick, Command::pick_iso), "install_page_buttons");
    }
    {
        quickmenu::Values w = v;
        w.picker.folder = "C:\\Games";
        w.picker.entries = {{"..", 0}, {"PS2", 1}, {"Sims 2.iso", 2}};
        Menu m(quickmenu::Mode::launcher);
        m.open();
        m.show(Page::pick_iso);
        m.press(Input::down, w);
        m.press(Input::down, w);
        const Action open = m.press(Input::confirm, w);
        const Action up = m.press(Input::back, w);
        check(is(open, Command::picker_open, 2) && up.cmd == Command::picker_open && up.value == -1, "picker_walk");
    }
    {
        quickmenu::Values w = v;
        Menu m(quickmenu::Mode::launcher);
        m.open();
        m.show(Page::progress);
        w.progress.steps = {{"Build tools", 1}, {"Source", 0}};
        m.press(Input::confirm, w);  // Cancel: asks
        const bool asked = m.dialog_open();
        m.press(Input::left, w);
        const Action cancel = m.press(Input::confirm, w);
        check(asked && is(cancel, Command::install_cancel), "progress_cancel_asks");
        w.progress.error = "no connection to github.com";
        check(is(m.press(Input::confirm, w), Command::install_retry), "progress_retry_after_error");
        w.progress.error.clear();
        w.progress.done = true;
        check(is(m.press(Input::confirm, w), Command::play), "progress_done_play");
    }
    {
        Menu m(quickmenu::Mode::launcher);
        m.set_update("v1.1");
        m.open();
        const Action update = m.press(Input::confirm, v);
        m.press(Input::down, v);
        const Action play = m.press(Input::confirm, v);
        Menu plain(quickmenu::Mode::launcher);
        plain.open();
        check(is(update, Command::update_now) && is(play, Command::play) && is(plain.press(Input::confirm, v), Command::play), "update_tile_on_main");
    }
    // ---- drawing ----
    {
        // every page and tab at many window sizes: the panel lies inside the window and its picture has the right size
        const int sizes[][2] = {{640, 480}, {800, 600}, {1280, 720}, {600, 1000}, {1600, 900}, {2560, 1440}, {3840, 2160}};
        quickmenu::Values w = v;
        for (int i = 0; i < 20; ++i) w.key_rows.push_back({"Left stick up", "W, Up"});
        bool ok = true;
        for (const auto &size : sizes) {
            for (int page = 0; page < 10; ++page) {
                Menu m;
                m.set_key_rows(20);
                m.open();
                if (page >= 1 && page <= 3) to_tab(m, w, page - 1);
                if (page == 4) {
                    to_tab(m, w, 1);
                    m.press(Input::confirm, w);
                    m.press(Input::confirm, w);  // the frame rate list, open
                }
                if (page == 5) {
                    to_tab(m, w, 2);
                    m.press(Input::right, w);
                    m.press(Input::confirm, w);
                    m.press(Input::down, w);
                    m.press(Input::confirm, w);  // the keys page
                }
                if (page >= 6) {
                    for (int i = 0; i < 30; ++i) w.list_rows.push_back({i % 7 ? "Some cheat" : "Heading", "60.0", "help", i % 7 == 0, i % 3 == 1, i % 2 == 0, i % 3 == 2});
                    for (int i = 0; i < (page == 6 ? 5 : 6); ++i) m.press(Input::down, w);
                    m.press(Input::confirm, w);  // the cheats / debug page
                    for (int i = 0; i < 20; ++i) m.press(Input::down, w);
                }
                if (page == 8) {  // the save states page
                    for (int i = 0; i < 3; ++i) m.press(Input::down, w);
                    m.press(Input::confirm, w);
                }
                if (page == 9) {  // a question over the settings
                    m.press(Input::up, w);
                    m.press(Input::confirm, w);
                }
                quickmenu::Picture p;
                m.draw(size[0], size[1], w, p);
                w.list_rows.clear();
                ok = ok && p.width > 0 && p.height > 0 && p.x >= 0 && p.y >= 0 && p.x + p.width <= size[0] && p.y + p.height <= size[1] &&
                     p.rgba.size() == static_cast<std::size_t>(p.width) * p.height;
            }
            quickmenu::Picture note;
            Menu::draw_note(size[0], size[1], "Quick save", note);
            ok = ok && note.width > 0 && note.x >= 0 && note.x + note.width <= size[0] && note.y + note.height <= size[1];
        }
        check(ok, "draw_fits_small_windows");
    }
    {
        // the panel's body: see-through periwinkle, as in the game's options panel (bottom-left corner area, inside the border)
        Menu m;
        to_tab(m, v, 0);
        quickmenu::Picture p;
        m.draw(1600, 900, v, p);
        bool found = false;
        for (int y = p.height * 3 / 4; y < p.height - 12 && !found; ++y)
            for (int x = 12; x < p.width / 2 && !found; ++x) {
                const std::uint32_t c = p.rgba[static_cast<std::size_t>(y) * p.width + x];
                const int r = c & 255, g = c >> 8 & 255, b = c >> 16 & 255, a = c >> 24;
                found = a >= 200 && a <= 230 && std::abs(r - 0x8F) <= 12 && std::abs(g - 0xA0) <= 12 && std::abs(b - 0xE0) <= 12;
            }
        check(found, "panel_is_translucent_periwinkle");
    }
    {
        Menu m;
        to_tab(m, v, 0);
        quickmenu::Picture p;
        m.draw(1600, 900, v, p);
        const quickmenu::Box b = m.hit_box(Command::look_filter, 1);
        m.click(b.x + b.w / 2, b.y + b.h / 2, v);
        check(b.w > 0 && b.h > 0 && m.inside() && m.pending(Command::look_filter) == 1, "click_matches_pad");
    }
    {
        // the keys page shows its first rows: one step down from the top must not scroll it (it did before its first drawing)
        Menu m;
        quickmenu::Values w = v;
        for (int i = 0; i < 14; ++i) w.key_rows.push_back({"Button", "Key"});
        m.set_key_rows(14);
        to_tab(m, w, 2);
        m.press(Input::right, w);
        m.press(Input::confirm, w);
        m.press(Input::down, w);
        m.press(Input::confirm, w);
        m.press(Input::down, w);
        quickmenu::Picture p;
        m.draw(1600, 900, w, p);
        check(m.key_row() == 1 && m.key_top() == 0, "keys_page_does_not_scroll_early");
    }
    // ---- local change: the launcher's Mods page (stage 3) ----
    {
        quickmenu::Values w = v;
        quickmenu::Values::ModRow a, b, c;
        a.name = "Pack", a.on = true;
        b.name = "Tweaks", b.on = false, b.warnings = {"Setting PS2_DEBAND also set by \"Pack\""};
        c.name = "Other game", c.can_on = false;
        w.mods.rows = {a, b, c};
        Menu m(quickmenu::Mode::launcher);
        m.open();
        m.press(Input::down, w);
        m.press(Input::down, w);
        const Action open = m.press(Input::confirm, w);
        const bool opened = is(open, Command::open_mods) && m.page() == Page::mods && m.focus() == 0;
        const Action zip = m.press(Input::confirm, w);      // Install from zip
        m.press(Input::down, w);                            // Pack's row, column On
        const Action toggle = m.press(Input::confirm, w);
        check(opened && is(zip, Command::mod_zip) && is(toggle, Command::mod_toggle) && toggle.value == 0 && m.focus() == 1, "mods_page_zip_and_toggle");
        m.press(Input::right, w);                           // Up
        const Action up_top = m.press(Input::confirm, w);   // the top cannot go up
        m.press(Input::right, w);                           // Down
        const Action down = m.press(Input::confirm, w);
        check(is(up_top, Command::none) && is(down, Command::mod_down) && down.value == 0 && m.focus() == 2, "mods_page_move_follows_the_mod");
        m.press(Input::down, w);                            // Other game
        m.press(Input::left, w);
        m.press(Input::left, w);                            // On
        const Action refused = m.press(Input::confirm, w);
        m.press(Input::right, w);
        m.press(Input::right, w);
        m.press(Input::right, w);                           // Delete
        const Action del = m.press(Input::confirm, w);
        const bool del_asks = m.dialog_open() && is(del, Command::none);
        m.press(Input::left, w);
        const Action yes = m.press(Input::confirm, w);
        check(is(refused, Command::none) && del_asks && is(yes, Command::mod_delete) && yes.value == 2, "mods_page_other_game_and_delete");
        m.press(Input::back, w);
        check(m.page() == Page::main && m.focus() == 2, "mods_page_back");
        quickmenu::Picture pic;
        m.press(Input::confirm, w);
        m.draw(1600, 900, w, pic);
        check(pic.width > 0, "mods_page_draws");
    }
    {  // the HD textures' tile on the launcher's main page while they are missing (part of the game, not a mod)
        Menu m(quickmenu::Mode::launcher);
        m.set_hd("Get the HD textures (98 MB)", false);
        m.open();
        const Action ask_hd = m.press(Input::confirm, v);
        const bool asked = m.dialog_open() && is(ask_hd, Command::none) && m.dialog_text().find("98 MB") != std::string::npos;
        const Action get = m.press(Input::confirm, v);      // (Yes is chosen already)
        m.press(Input::down, v);
        const Action play = m.press(Input::confirm, v);
        check(asked && is(get, Command::get_hd) && is(play, Command::play), "hd_tile_asks_then_downloads");
        m.set_hd("Downloading the HD textures: 40 of 97 MB", true);
        m.press(Input::up, v);
        m.press(Input::confirm, v);
        const bool stop_asked = m.dialog_open() && m.dialog_text().find("Stop") != std::string::npos;
        m.press(Input::left, v);
        const Action stop = m.press(Input::confirm, v);
        m.set_hd("", false);
        m.open();
        const Action plain = m.press(Input::confirm, v);
        check(stop_asked && is(stop, Command::hd_cancel) && is(plain, Command::play), "hd_tile_stop_and_gone");
        m.set_update("v2");
        m.set_hd("Get the HD textures (98 MB)", false);
        m.open();
        check(is(m.press(Input::confirm, v), Command::update_now), "hd_tile_gives_way_to_an_update");
    }
    // ---- local change: the Back button (top left of every page but the main and install pages), for the mouse ----
    {
        quickmenu::Picture pic;
        const auto click_back = [&](Menu &m, const quickmenu::Values &w) {
            m.draw(1600, 900, w, pic);
            const quickmenu::Box b = m.back_box();
            return b.w > 0 ? m.click(b.x + b.w / 2, b.y + b.h / 2, w) : Action{Command::count_};
        };
        Menu m(quickmenu::Mode::launcher);
        m.open();
        m.draw(1600, 900, v, pic);
        const bool none_on_main = m.back_box().w == 0;
        m.press(Input::down, v);
        m.press(Input::confirm, v);  // Memory cards
        const Action from_cards = click_back(m, v);
        check(none_on_main && is(from_cards, Command::none) && m.page() == Page::main && m.focus() == 1, "back_button_cards");
        m.press(Input::down, v);
        m.press(Input::confirm, v);  // Mods
        click_back(m, v);
        const bool from_mods = m.page() == Page::main && m.focus() == 2;
        m.press(Input::down, v);
        m.press(Input::confirm, v);  // Settings, into a category
        m.press(Input::confirm, v);
        const bool was_inside = m.inside();
        click_back(m, v);
        check(from_mods && was_inside && m.page() == Page::main && m.focus() == 3, "back_button_settings_from_inside_a_category");
        m.press(Input::confirm, v);  // Settings again: a pick not applied -> the question comes, the page stays
        m.press(Input::confirm, v);
        m.press(Input::left, v);
        m.press(Input::confirm, v);
        if (m.change_count() == 0) {  // (it was the first choice already)
            m.press(Input::right, v);
            m.press(Input::confirm, v);
        }
        const int changes = m.change_count();
        click_back(m, v);
        check(changes == 1 && m.dialog_open() && m.page() == Page::settings, (std::string("back_button_settings_asks_about_changes: changes ") + std::to_string(changes) + " dialog " + std::to_string(m.dialog_open()) + " page " + std::to_string(static_cast<int>(m.page()))).c_str());
        Menu q(quickmenu::Mode::launcher);
        q.open();
        q.show(Page::pick_iso);
        quickmenu::Values w = v;
        w.picker.folder = "C:\\Games";
        w.picker.entries = {{"..", 0}, {"Mods", 1}};
        const Action leave = click_back(q, w);
        check(is(leave, Command::picker_open) && leave.value == -2, "back_button_picker_leaves");
        q.show(Page::install);
        q.draw(1600, 900, w, pic);
        check(q.back_box().w == 0, "back_button_not_on_install");
        Menu g;  // the game's quick menu: the cheats page
        g.open();
        for (int i = 0; i < 6; ++i) g.press(Input::down, v);
        g.press(Input::confirm, v);
        const bool on_list = g.list_page();
        click_back(g, v);
        check(on_list && g.page() == Page::main, "back_button_game_pages");
    }
    std::printf(g_failed ? "%d FAILED\n" : "all ok\n", g_failed);
    return g_failed ? 1 : 0;
}
