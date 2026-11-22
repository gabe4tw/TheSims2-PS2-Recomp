// Local change (the remaster): Sims2Launcher.exe - Play, Memory cards, Mods, Settings, Quit, in the quick menu's look.
// It holds no game code: Play starts build\native-sims2\ps2run.exe with play-sims2.cmd's arguments (launcher.ini), hides while the game
// runs and comes back after. Memory cards and Settings work on the same files as the game's quick menu (games\sims2\cards,
// remaster.ini, keys.ini), read again whenever the game has ended. Design: C:\PS2Native\plans\2026-11-17-launcher-design.md (stage 1).

#include "card_library.hpp"
#include "card_page.hpp"
#include "keymap.hpp"
#include "launcher_config.hpp"
#include "launcher_frame.hpp"
#include "hd_pack.hpp"
#include "launcher_install.hpp"
#include "launcher_mods.hpp"
#include "mod_library.hpp"
#include "launcher_update.hpp"
#include "launcher_os.hpp"
#include "menu_sound.hpp"
#include "quickmenu.hpp"
#include "remaster_ini.hpp"
#include "settings_fill.hpp"
#include "webcam.hpp"

#include "ps2/os.hpp"

#ifdef _WIN32
#include <windows.h>
#endif
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using quickmenu::Command;
using quickmenu::Input;

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_panel = nullptr;
int g_panel_w = 0, g_panel_h = 0;
SDL_AudioDeviceID g_audio = 0;
// the borderless window: a Windows layered window that shows only the panel (g_layered); without it (another system) the old way
bool g_layered = false;
SDL_Rect g_drag{};  // the title tab of the panel drawn last, in window pixels: dragging it moves the window
std::vector<std::uint32_t> g_frame;
quickmenu::Menu g_menu(quickmenu::Mode::launcher);
bool g_quit = false;

LaunchConfig g_config;
bool g_have_config = false;
std::string g_config_problem;
LauncherFiles g_files;
std::string g_cards[2];  // the card names in slot 1 / 2 ("" = empty), kept in remaster.ini
std::vector<std::string> g_webcams;
std::vector<SDL_GameController *> g_pads;
std::string g_status;  // the main page's line

// ---- files ----

std::string read_text(const std::string &file) {
    std::string text;
    if (std::FILE *f = ps2::os::open(file, "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    return text;
}

// ---- local change (stage 3): the mods (host/mod_library.hpp, host/launcher_mods.hpp) ----
mods::Library g_mods;
quickmenu::Values::Mods g_mod_view;  // the Mods page's rows, made again after each change
bool g_pick_zip = false;             // the picker chooses a mod's .zip (else a disc image)
std::thread g_mod_worker;            // getting the HD textures (host/hd_pack.hpp; part of the game, not a mod)
bool g_hd_present = true;            // the game folder has them (read again with the files)
std::atomic<bool> g_mod_busy{false}, g_mod_cancel{false}, g_mod_finished{false};
std::mutex g_mod_lock;
std::string g_mod_line, g_mod_result;  // (g_mod_lock) the running line; what came of it
double g_mod_fraction = -1;

std::string mods_dir() { return g_have_config ? under_root(g_config, g_config.mods) : std::string(); }

// the mods folder read again (zips dropped into it unpacked first); their settings go over remaster.ini's values
void refresh_mods(bool unpack_zips) {
    if (!g_have_config) return;
    std::string why;
    if (unpack_zips)
        for (const std::string &line : launchermods::unpack_new_zips(mods_dir())) {
            g_status = line;
            std::fprintf(stderr, "[launcher] mods: %s\n", line.c_str());
        }
    if (!mods::load(mods_dir(), g_mods, &why)) std::fprintf(stderr, "[launcher] mods: %s\n", why.c_str());
    launchermods::apply_settings(g_mods);
    g_mod_view = launchermods::rows(g_mods);
}

void reload() {
    if (!g_have_config) return;
    g_files = load_files(g_config);
    for (int slot = 0; slot < 2; ++slot) g_cards[slot] = g_files.extra.card[slot] == "none" ? std::string() : g_files.extra.card[slot];
    refresh_mods(true);  // local change (stage 3)
    g_hd_present = hdpack::installed(under_root(g_config, g_config.game_dir));
}

void write_remaster() {
    g_files.extra.card_library = true;
    for (int slot = 0; slot < 2; ++slot) g_files.extra.card[slot] = g_cards[slot].empty() ? "none" : g_cards[slot];
    remaster::write(g_files.remaster_ini, g_files.extra);
}

void write_keys() {
    std::string text = read_text(g_files.keys_ini);
    if (text.empty()) text = keymap::default_file_text();
    if (g_files.look >= 0) text = keymap::with_look(text, g_files.look);
    if (g_files.scale > 0) text = keymap::with_scale(text, g_files.scale);
    if (g_files.sharp >= 0) text = keymap::with_sharp_2d(text, g_files.sharp);
    std::string why;
    if (!ps2::os::replace_file(g_files.keys_ini, text.data(), text.size(), &why)) std::fprintf(stderr, "[launcher] cannot write %s (%s)\n", g_files.keys_ini.c_str(), why.c_str());
}

// ---- controllers (ids as the game's window gives them: GUID # which one of that GUID) ----

std::string pad_guid(SDL_GameController *pad) {
    char text[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pad)), text, sizeof text);
    return text;
}

std::string pad_id(SDL_GameController *pad) {
    const std::string guid = pad_guid(pad);
    int nth = 0;
    for (SDL_GameController *other : g_pads) {
        if (other == pad) break;
        if (pad_guid(other) == guid) ++nth;
    }
    return guid + "#" + std::to_string(nth);
}

std::vector<std::string> player_ids(int player) {
    std::vector<std::string> ids = {"auto", player ? "none" : "all", "keyboard"};  // (local change: "keyboard" = Keyboard & mouse)
    for (SDL_GameController *pad : g_pads) ids.push_back(pad_id(pad));
    const std::string &chosen = g_files.extra.player[player];
    if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) ids.push_back(chosen);
    return ids;
}

std::string player_word(int player, const std::string &id) {
    if (id == "auto") return player ? "Automatic (the next free controller)" : "Automatic (the first controller)";
    if (id == "all") return "All controllers";
    if (id == "none") return "Nobody";
    if (id == "keyboard") return "Keyboard & mouse";
    for (std::size_t n = 0; n < g_pads.size(); ++n)
        if (pad_id(g_pads[n]) == id) {
            const char *name = SDL_GameControllerName(g_pads[n]);
            std::string word = std::to_string(n + 1) + ": " + (name ? name : "Controller");
            if (SDL_GameControllerGetType(g_pads[n]) == SDL_CONTROLLER_TYPE_VIRTUAL) word += " (virtual)";
            return word;
        }
    return "A controller that is not connected";
}

std::vector<std::string> webcam_ids() {
    std::vector<std::string> ids = {"none"};
    for (const std::string &name : g_webcams) ids.push_back(name);
    if (std::find(ids.begin(), ids.end(), g_files.extra.webcam) == ids.end()) ids.push_back(g_files.extra.webcam);
    return ids;
}

std::string webcam_word(const std::string &id) {
    if (id == "none") return "None (unplugged)";
    if (id == "test") return "Test picture";
    if (id == "auto") return g_webcams.empty() ? "First webcam (none found)" : "First webcam";
    if (std::find(g_webcams.begin(), g_webcams.end(), id) == g_webcams.end()) return id + " (not found)";
    return id;
}

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

// ---- the pages ----

cardpage::Slots slots() {
    return cardpage::file_slots(g_cards, [] { write_remaster(); });
}

SettingsState settings_state() {
    SettingsState st;
    if (g_files.look >= 0) st.look = g_files.look;
    st.scale_now = st.scale_next = g_files.scale > 0 ? g_files.scale : 1;
    st.sharp_now = st.sharp_next = g_files.sharp >= 0 ? g_files.sharp : 1;
    st.window_mode = g_files.extra.window_mode;
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(std::max(0, SDL_GetWindowDisplayIndex(g_window)), &mode) == 0 && mode.refresh_rate >= 50) st.screen_hz = mode.refresh_rate;
    st.texture_pack = g_have_config && fs::is_directory(ps2::os::path(under_root(g_config, g_config.game_dir)) / "textures" / "enhanced");
    for (const mods::Mod &m : g_mods.mods) st.texture_pack = st.texture_pack || (m.enabled && m.textures);  // local change (stage 3)
    st.texture_pack = st.texture_pack || g_hd_present;  // (the HD textures)
    st.screen_sizes = screen_sizes();
    int w = 0, h = 0;
    if (std::sscanf(g_files.extra.screen_res.c_str(), "%dx%d", &w, &h) == 2)
        for (std::size_t i = 0; i < st.screen_sizes.size(); ++i)
            if (st.screen_sizes[i] == std::make_pair(w, h)) st.screen_choice = static_cast<int>(i);
    for (int player = 0; player < 2; ++player) {
        st.player_ids[player] = player_ids(player);
        for (const std::string &id : st.player_ids[player]) st.player_words[player].push_back(player_word(player, id));
        st.player_choice[player] = g_files.extra.player[player];
    }
    st.webcam_ids = webcam_ids();
    for (const std::string &id : st.webcam_ids) st.webcam_words.push_back(webcam_word(id));
    st.webcam_now = st.webcam_next = g_files.extra.webcam;
    return st;
}

std::string slots_line() {
    const auto word = [](const std::string &n) { return n.empty() ? std::string("empty") : n; };
    return "Memory cards: slot 1 " + word(g_cards[0]) + ", slot 2 " + word(g_cards[1]);
}

// ---- local change (stage 2): installing on a player's PC, and updates ----
// Three ways the launcher starts: (1) launcher.ini without installed= (the developer's own tree): as stage 1; (2) launcher.ini with
// installed=<version>: the normal pages plus an update check; (3) no launcher.ini: the install pages (the disc image next to the launcher
// is used when it is the right one; otherwise the player picks one).
enum class Mode { dev, installed, install };
Mode g_mode = Mode::dev;
Layout g_layout;
InstallState g_install;
std::string g_iso_line;                   // what is wrong with the chosen disc image ("" = right)
std::string g_pick_folder;                // the picker's folder ("" = the drives)
std::vector<std::pair<std::string, int>> g_pick_entries;
std::thread g_worker;
std::atomic<bool> g_cancel{false}, g_working{false};
std::mutex g_progress_lock;
quickmenu::Values::Progress g_progress;   // filled by the worker
std::atomic<bool> g_finished{false};      // the worker ended (success or failure): the main loop takes it up
bool g_finished_ok = false;
bool g_updating = false;
std::string g_update;                     // a newer release ("" = none / not checked)
std::thread g_update_check;

std::string size_text(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (std::uint64_t(1) << 30)) std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / (1 << 30));
    else std::snprintf(text, sizeof text, "%.0f MB", std::max(1.0, static_cast<double>(bytes) / (1 << 20)));
    return text;
}

void choose_iso(const std::string &iso) {
    g_install.iso = iso;
    std::string boot, why;
    const DiscKind kind = check_disc(iso, boot, why);
    g_iso_line = kind == DiscKind::unreadable ? why : disc_message(kind);
    save_state(g_layout, g_install);
}

void fill_install(quickmenu::Values &v) {
    v.install.iso = g_install.iso;
    v.install.disc_line = g_install.iso.empty() ? std::string() : g_iso_line;
    v.install.can_install = !g_install.iso.empty() && g_iso_line.empty();
    std::uint64_t total = 0;
    for (const Download &t : tool_downloads()) {
        std::error_code e;
        if (fs::exists(ps2::os::path(g_layout.tools) / ps2::os::path(t.folder) / ".unpacked", e)) continue;
        v.install.downloads.push_back({t.name, size_text(t.size)});
        total += t.size;
    }
    v.install.downloads.push_back({"The remaster's source and the build's libraries (SDL2, wgpu, DXC)", "about 70 MB"});
    total += std::uint64_t(70) << 20;
    if (!hdpack::installed(g_layout.data)) {  // local change (stage 3)
        v.install.downloads.push_back({"HD textures (SomberTwilight's texture pack)", hdpack::size_text()});
        total += hdpack::Info{}.size;
    }
    v.install.total = "About " + size_text(total) + " to download; " + size_text(space_needed()) + " of free space needed.";
    const std::uint64_t free = free_bytes(g_layout.base);
    if (free) v.install.free = size_text(free) + " free here.";
}

void fill_picker() {
    g_pick_entries.clear();
    std::error_code e;
    if (g_pick_folder.empty()) {
#ifdef _WIN32
        const DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; ++d)
            if (drives & (1u << d)) g_pick_entries.push_back({std::string(1, static_cast<char>('A' + d)) + ":\\", 1});
#endif
        return;
    }
    g_pick_entries.push_back({"..", 0});
    std::vector<std::string> folders, isos;
    for (fs::directory_iterator it(ps2::os::path(g_pick_folder), fs::directory_options::skip_permission_denied, e), end; !e && it != end; it.increment(e)) {
        std::error_code f;
        const std::string name = ps2::os::utf8(it->path().filename());
        if (name.empty() || name[0] == '$' || name[0] == '.') continue;
        if (it->is_directory(f)) folders.push_back(name);
        else if (it->is_regular_file(f)) {
            std::string ext = ps2::os::utf8(it->path().extension());
            for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == (g_pick_zip ? ".zip" : ".iso")) isos.push_back(name);  // (local change: a mod's .zip for the Mods page)
        }
    }
    const auto by_name = [](const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(isos.begin(), isos.end(), by_name);
    for (const std::string &f : folders) g_pick_entries.push_back({f, 1});
    for (const std::string &i : isos) g_pick_entries.push_back({i, 2});
}

void picker_open(int row) {
    const auto up = [] {
        const fs::path p = ps2::os::path(g_pick_folder);
        g_pick_folder = p.has_relative_path() && p.relative_path() != p.filename() ? ps2::os::utf8(p.parent_path())
                        : p.has_relative_path()                                     ? ps2::os::utf8(p.root_path())
                                                                                   : std::string();
    };
    if (row < 0) {  // Triangle: a folder up; from the drives back to the install page (the Mods page). -2: the Back button: leave at once
        if (g_pick_folder.empty() || row == -2) {
            g_menu.show(g_pick_zip ? quickmenu::Page::mods : quickmenu::Page::install);
            return;
        }
        up();
    } else if (row < static_cast<int>(g_pick_entries.size())) {
        const auto &entry = g_pick_entries[row];
        if (entry.second == 0) up();
        else if (entry.second == 1) g_pick_folder = g_pick_folder.empty() ? entry.first : ps2::os::utf8(ps2::os::path(g_pick_folder) / ps2::os::path(entry.first));
        else if (g_pick_zip) {  // local change (stage 3): a mod's zip
            const std::string zip = ps2::os::utf8(ps2::os::path(g_pick_folder) / ps2::os::path(entry.first));
            std::string folder, why;
            const bool ok = launchermods::install_zip(mods_dir(), zip, folder, why);
            refresh_mods(false);
            g_menu.show(quickmenu::Page::mods);
            g_menu.set_status(ok ? "Installed \"" + entry.first + "\" as the mod \"" + folder + "\": it is on, at the top." : "Not installed: " + why);
            return;
        } else {
            choose_iso(ps2::os::utf8(ps2::os::path(g_pick_folder) / ps2::os::path(entry.first)));
            g_menu.show(quickmenu::Page::install);
            return;
        }
    }
    fill_picker();
    g_menu.show(quickmenu::Page::pick_iso);
}

// the progress page's step list from how far the install got
void set_steps(Step reached, bool failed) {
    static const char *const kNames[] = {"Free space", "Build tools (download and unpack)", "The remaster's source", "Your data folder (cards, settings, HD textures)",
                                         "Building the game from your disc"};
    std::lock_guard<std::mutex> hold(g_progress_lock);
    g_progress.steps.clear();
    for (int i = 0; i < 5; ++i) {
        const int r = static_cast<int>(reached);
        g_progress.steps.push_back({kNames[i], i < r ? 2 : i == r ? (failed ? 3 : 1) : 0});
    }
    if (g_updating) g_progress.steps.erase(g_progress.steps.begin(), g_progress.steps.begin() + 2);  // (an update starts at the source)
}

InstallSetup install_setup() {
    InstallSetup setup;
    setup.source_url = ps2::os::env("PS2_LAUNCHER_SOURCE");  // (tests: a local source zip)
    setup.hd_textures = true;                                // local change (stage 3): the HD textures come with the game
    setup.hd_local = ps2::os::env("PS2_LAUNCHER_HDPACK");    // (tests: a local copy of the archive)
    return setup;
}

void start_worker(bool update) {
    if (g_working.load()) return;
    if (g_worker.joinable()) g_worker.join();
    g_cancel = false;
    g_finished = false;
    g_working = true;
    g_updating = update;
    {
        std::lock_guard<std::mutex> hold(g_progress_lock);
        g_progress = {};
    }
    set_steps(update ? Step::source : g_install.reached, false);
    g_menu.show(quickmenu::Page::progress);
    g_worker = std::thread([update] {
        InstallState s = g_install;
        bool ok = false;
        std::string version = update ? g_update : ps2::os::env("PS2_LAUNCHER_VERSION");
        if (version.empty() && !update && !s.version.empty()) version = s.version;  // (an install that was stopped goes on with its version)
        if (version.empty()) {
            std::string why;
            version = latest_release(kLatestReleaseUrl, why);
            if (version.empty()) s.error = "Could not find the newest release of the remaster (" + why + "). Is this PC online?";
        }
        if (!version.empty()) {
            const Report report = [&](const std::string &line, double fraction) {
                std::lock_guard<std::mutex> hold(g_progress_lock);
                g_progress.line = line;
                if (fraction >= 0) g_progress.fraction = fraction;
            };
            const auto steps_from = [&](Step r) { set_steps(r, false); };
            if (update) {
                ok = run_update(g_layout, s, version, install_setup(), report, g_cancel);
            } else {
                s.version = version;
                InstallSetup setup = install_setup();
                // the step list follows the state as the install goes (install.ini is saved after every step)
                std::thread watch([&] {
                    while (g_working.load()) {
                        InstallState now;
                        if (load_state(g_layout, now)) steps_from(now.reached);
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    }
                });
                ok = run_install(g_layout, s, setup, report, g_cancel);
                if (ok) {
                    std::string why;
                    if (!write_installed_config(g_layout, s, why)) {
                        ok = false;
                        s.error = why;
                    }
                }
                g_working = false;
                watch.join();
            }
        }
        g_install = s;
        g_finished_ok = ok;
        set_steps(ok ? Step::done : s.reached, !ok);
        {
            std::lock_guard<std::mutex> hold(g_progress_lock);
            g_progress.error = ok ? std::string() : (s.error.empty() ? std::string("stopped") : s.error);
            g_progress.done = ok;
            if (ok) g_progress.fraction = 1.0;
        }
        g_working = false;
        g_finished = true;
    });
}

// the launcher.ini an install wrote: from now on the normal pages
bool open_installed(const std::string &config_file);

void install_finished() {
    if (!g_finished.exchange(false)) return;
    if (g_worker.joinable()) g_worker.join();
    if (!g_finished_ok) return;  // (the progress page shows the error and Try again)
    open_installed(ps2::os::utf8(ps2::os::path(g_layout.base) / "launcher.ini"));
    if (g_updating) {
        g_update.clear();
        g_menu.set_update("");
        g_status = "Updated to " + g_install.version + ".";
        g_menu.show(quickmenu::Page::main);
    }
}

quickmenu::Values values() {
    quickmenu::Values v;
    v.picker.folder = g_pick_folder;  // (local change: also the Mods page's zip picker)
    v.picker.entries = g_pick_entries;
    v.picker.zips = g_pick_zip;
    v.mods = g_mod_view;
    if (g_mode == Mode::install || g_working.load() || g_menu.page() == quickmenu::Page::progress) {
        fill_install(v);
        std::lock_guard<std::mutex> hold(g_progress_lock);
        v.progress = g_progress;
    }
    fill_settings(v, settings_state());
    if (g_have_config) cardpage::fill(v, g_files.card_library, slots());
    g_menu.set_main_status(!g_config_problem.empty() ? g_config_problem : !g_status.empty() ? g_status : g_have_config ? slots_line() : std::string());
    return v;
}

// one Settings change (Apply): into the files' values
void apply_one(const quickmenu::Action &a, const quickmenu::Values &v) {
    static constexpr Command kOptions[remaster::kOptionCount] = {Command::opt_frame_rate, Command::opt_ui_width, Command::opt_load_turbo, Command::opt_disc_speed,
                                                                 Command::opt_pad_merge,  Command::opt_deband,   Command::opt_textures,   Command::opt_vsync,
                                                                 Command::opt_skip_intro, Command::opt_aspect, Command::opt_aa, Command::opt_mouse_mode};
    for (int i = 0; i < remaster::kOptionCount; ++i)
        if (a.cmd == kOptions[i] && a.value >= 0 && a.value < static_cast<int>(remaster::g_options[i].values.size())) remaster::g_options[i].next = remaster::g_options[i].values[a.value];
    const int look = g_files.look >= 0 ? g_files.look : keymap::kDefaultLook;
    switch (a.cmd) {
    case Command::look_filter: g_files.look = (a.value + 3) % 4 * 2 + look % 2; break;  // (Off, Crisp, Soft, Sharp = look / 2: 3, 0, 1, 2)
    case Command::opt_aa: g_files.look = look / 2 * 2 + (remaster::g_options[remaster::kAaOption].next == 1 ? 1 : 0); break;  // (the old bit, for older runners)
    case Command::scale: g_files.scale = a.value + 1; break;
    case Command::sharp_2d: g_files.sharp = a.value; break;
    case Command::fullscreen: g_files.extra.window_mode = a.value; break;
    case Command::opt_screen_res: {
        const auto sizes = screen_sizes();
        if (a.value <= 0 || a.value >= static_cast<int>(sizes.size())) g_files.extra.screen_res.clear();
        else g_files.extra.screen_res = std::to_string(sizes[a.value].first) + "x" + std::to_string(sizes[a.value].second);
        break;
    }
    case Command::opt_player1:
    case Command::opt_player2: {
        const int player = a.cmd == Command::opt_player2 ? 1 : 0;
        const std::vector<std::string> ids = player_ids(player);
        if (a.value >= 0 && a.value < static_cast<int>(ids.size())) g_files.extra.player[player] = ids[a.value];
        if (g_files.extra.player[player] == "keyboard" && g_files.extra.player[1 - player] == "keyboard") g_files.extra.player[1 - player] = "auto";  // (one keyboard)
        remaster::g_options[4].next = g_files.extra.player[0] == "all" ? 1 : 0;  // (PS2_PAD_MERGE, for older runners)
        break;
    }
    case Command::opt_webcam: {
        const std::vector<std::string> ids = webcam_ids();
        if (a.value >= 0 && a.value < static_cast<int>(ids.size())) g_files.extra.webcam = ids[a.value];
        break;
    }
    default: break;
    }
    (void)v;
}

void show_panel();

void play() {
    if (!g_have_config) return;
    const std::string runner = under_root(g_config, g_config.runner);
    std::string swap;
    const std::string blocked = prepare_play(g_config, runner_running(runner), &swap);
    if (!blocked.empty()) {
        g_status = blocked;
        return;
    }
    if (g_mod_busy.load()) {  // local change (stage 3)
        g_status = "The HD textures are still being downloaded. Play when they are done.";
        return;
    }
    ps2::os::set_env("PS2_MODS", mods_dir());  // local change (stage 3): the game reads the enabled mods from there
    std::fprintf(stderr, "[launcher] starting the game: %s\n", runner.c_str());
    SDL_HideWindow(g_window);
    int code = 0;
    std::string why;
    const bool started = start_and_wait(runner, game_arguments(g_config), g_config.root, code, why);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);  // (presses made while the game ran belong to the game)
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    reload();  // the game's quick menu may have changed cards and settings
    g_menu.open();
    if (!started) g_status = why;
    else if (code != 0) g_status = "The game ended with an error (code " + std::to_string(code) + "). Its log is in " + g_config.game_dir + "\\logs.";
    else g_status = swap;
    if (started && code == 0 && g_mode == Mode::installed) remove_other_versions(g_layout, installed_version(g_layout));  // (after an update: the old one goes)
    std::fprintf(stderr, "[launcher] the game ended (code %d)\n", code);
}

bool game_running() { return g_have_config && runner_running(under_root(g_config, g_config.runner)); }

// local change (stage 3): the HD textures (part of the game: host/hd_pack.hpp), downloaded (or PS2_LAUNCHER_HDPACK's local copy),
// checked and unpacked into the game folder's textures\hd; on a thread, the progress on the main page's tile
void start_hd_worker() {
    if (!g_have_config || g_mod_busy.load()) return;
    if (g_mod_worker.joinable()) g_mod_worker.join();
    const std::string game_dir = under_root(g_config, g_config.game_dir);
    const std::string downloads = g_mode == Mode::installed ? g_layout.downloads : ps2::os::utf8(ps2::os::path(game_dir) / "textures" / ".downloads");
    const std::string local = ps2::os::env("PS2_LAUNCHER_HDPACK");
    g_mod_busy = true;
    g_mod_cancel = false;
    {
        std::lock_guard<std::mutex> hold(g_mod_lock);
        g_mod_line = "Downloading the HD textures...";
        g_mod_fraction = 0;
    }
    std::fprintf(stderr, "[launcher] HD textures: %s\n", local.empty() ? hdpack::download_url().c_str() : local.c_str());
    g_mod_worker = std::thread([game_dir, downloads, local] {
        std::string file, why;
        bool ok = hdpack::fetch(downloads, local,
                                [](std::uint64_t got, std::uint64_t total) {
                                    std::lock_guard<std::mutex> hold(g_mod_lock);
                                    g_mod_fraction = total ? static_cast<double>(got) / static_cast<double>(total) : -1;
                                    g_mod_line = "Downloading the HD textures: " + std::to_string(got >> 20) + " of " +
                                                 std::to_string((total ? total : hdpack::Info{}.size) >> 20) + " MB  (Cross: stop)";
                                    return !g_mod_cancel.load();
                                },
                                file, why);
        if (ok) {
            {
                std::lock_guard<std::mutex> hold(g_mod_lock);
                g_mod_line = "Unpacking the HD textures...";
                g_mod_fraction = -1;
            }
            ok = hdpack::install(game_dir, file, why);
        }
        std::lock_guard<std::mutex> hold(g_mod_lock);
        g_mod_result = ok ? std::string("The HD textures are in. Settings > Textures switches between them and the original ones.")
                          : g_mod_cancel.load() ? std::string("The download of the HD textures was stopped.") : "The HD textures were not installed: " + why;
        std::fprintf(stderr, "[launcher] HD textures: %s\n", g_mod_result.c_str());
        g_mod_line.clear();
        g_mod_fraction = -1;
        g_mod_busy = false;
        g_mod_finished = true;
    });
}

void hd_worker_finished() {
    if (!g_mod_finished.exchange(false)) return;
    if (g_mod_worker.joinable()) g_mod_worker.join();
    g_hd_present = hdpack::installed(under_root(g_config, g_config.game_dir));
    std::lock_guard<std::mutex> hold(g_mod_lock);
    g_status = g_mod_result;
}

void act(const quickmenu::Action &a) {
    const bool changes_files = a.cmd == Command::apply || a.cmd == Command::card_new || a.cmd == Command::card_insert || a.cmd == Command::card_eject ||
                               a.cmd == Command::card_rename || a.cmd == Command::card_copy || a.cmd == Command::card_delete || a.cmd == Command::mod_toggle ||
                               a.cmd == Command::mod_up || a.cmd == Command::mod_down || a.cmd == Command::mod_delete || a.cmd == Command::mod_zip || a.cmd == Command::get_hd;
    if (changes_files) {
        const std::string blocked = files_blocker(game_running());
        if (!blocked.empty()) {
            g_menu.set_status(blocked);
            return;  // (Apply's picks stay pending; nothing is written)
        }
    }
    switch (a.cmd) {
    case Command::play:
        if (g_mode == Mode::install) break;
        if (g_menu.page() == quickmenu::Page::progress) g_menu.show(quickmenu::Page::main);
        play();
        break;
    case Command::pick_iso:
        g_pick_zip = false;
        g_pick_folder = g_install.iso.empty() ? g_layout.base : ps2::os::utf8(ps2::os::path(g_install.iso).parent_path());
        fill_picker();
        g_menu.show(quickmenu::Page::pick_iso);
        break;
    case Command::picker_open: picker_open(a.value); break;
    case Command::install_start:
    case Command::install_retry: start_worker(g_updating && a.cmd == Command::install_retry); break;
    case Command::install_cancel: g_cancel = true; break;
    case Command::update_now:
        if (!game_running()) start_worker(true);
        else g_status = files_blocker(true);
        break;
    case Command::quit: g_quit = true; break;
    case Command::apply: {
        const quickmenu::Values v = values();
        const std::vector<quickmenu::Action> changes = g_menu.changes();
        g_menu.clear_changes();
        for (const quickmenu::Action &c : changes) apply_one(c, v);
        write_keys();
        write_remaster();
        const std::string count = std::to_string(changes.size()) + (changes.size() == 1 ? " change" : " changes");
        g_menu.set_status(count + " saved. They are used when the game starts.");
        std::fprintf(stderr, "[launcher] %s saved\n", count.c_str());
        break;
    }
    case Command::card_new: case Command::card_insert: case Command::card_eject: case Command::card_rename: case Command::card_copy:
    case Command::card_delete: {
        if (!g_have_config) break;
        const std::string line = cardpage::act(a, g_menu.edit_text(), g_files.card_library, slots());
        if (!line.empty()) g_menu.set_status(line);
        break;
    }
    // local change (stage 3): the Mods page
    case Command::mod_toggle: case Command::mod_up: case Command::mod_down: case Command::mod_delete: {
        if (!g_have_config || a.value < 0 || a.value >= static_cast<int>(g_mods.mods.size())) break;
        const std::string name = g_mods.mods[static_cast<std::size_t>(a.value)].name;
        const bool was_on = g_mods.mods[static_cast<std::size_t>(a.value)].enabled;
        std::string why;
        bool ok = false;
        if (a.cmd == Command::mod_toggle) ok = mods::set_enabled(g_mods, a.value, !was_on, &why);
        else if (a.cmd == Command::mod_delete) ok = mods::remove(g_mods, a.value, &why);
        else ok = mods::move(g_mods, a.value, a.cmd == Command::mod_up ? -1 : 1, &why);
        refresh_mods(false);
        if (!ok) g_menu.set_status(why.empty() ? "Nothing changed." : why);
        else if (a.cmd == Command::mod_toggle) g_menu.set_status("\"" + name + "\" is " + (was_on ? "off." : "on.") + " Used when the game starts.");
        else if (a.cmd == Command::mod_delete) g_menu.set_status("\"" + name + "\" is deleted.");
        std::fprintf(stderr, "[launcher] mods: %s %s%s\n", a.cmd == Command::mod_toggle ? "switched" : a.cmd == Command::mod_delete ? "deleted" : "moved",
                     name.c_str(), ok ? "" : (" failed: " + why).c_str());
        break;
    }
    case Command::mod_zip: {
        g_pick_zip = true;
        const fs::path downloads = ps2::os::path(ps2::os::env("USERPROFILE")) / "Downloads";
        std::error_code e;
        g_pick_folder = !ps2::os::env("USERPROFILE").empty() && fs::is_directory(downloads, e) ? ps2::os::utf8(downloads) : std::string();
        fill_picker();
        g_menu.show(quickmenu::Page::pick_iso);
        break;
    }
    case Command::get_hd: start_hd_worker(); break;
    case Command::hd_cancel: g_mod_cancel = true; break;
    default: break;  // (page changes: the menu does them itself; settings wait for Apply)
    }
}

void press(Input in) {
    menusound::play(in == Input::confirm ? menusound::Clip::select : in == Input::back ? menusound::Clip::back : menusound::Clip::move);
    if (g_menu.page() == quickmenu::Page::main && in != Input::confirm) g_status.clear();
    const quickmenu::Values v = values();
    act(g_menu.press(in, v));
}

// ---- drawing ----

void draw() {
    int w = 0, h = 0;
    quickmenu::Picture p;
    const quickmenu::Values v = values();
    if (g_layered) {
        SDL_GetWindowSize(g_window, &w, &h);
        g_menu.draw(w, h, v, p);
        const float k = static_cast<float>(p.width) / std::max(1.0f, g_menu.panel_units());
        g_drag = {p.x, p.y, p.width, static_cast<int>(quickmenu::Menu::kTitleUnits * k)};
        compose_layered(p, w, h, g_frame);
        if (present_layered(g_window, g_frame, w, h)) return;
        std::fprintf(stderr, "[launcher] the see-through window did not work; drawing the plain way\n");
        g_layered = false;
        SDL_SetWindowBordered(g_window, SDL_TRUE);
    }
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) return;
    SDL_GetRendererOutputSize(g_renderer, &w, &h);
    // a plain dark periwinkle gradient behind the panel (no game art)
    for (int band = 0; band < 48; ++band) {
        const int t = band * 255 / 47;
        SDL_SetRenderDrawColor(g_renderer, static_cast<Uint8>(34 + t / 12), static_cast<Uint8>(40 + t / 10), static_cast<Uint8>(86 + t / 5), 255);
        SDL_Rect r{0, h * band / 48, w, h / 48 + 2};
        SDL_RenderFillRect(g_renderer, &r);
    }
    g_menu.draw(w, h, v, p);
    if (!g_panel || g_panel_w != p.width || g_panel_h != p.height) {
        if (g_panel) SDL_DestroyTexture(g_panel);
        g_panel = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, p.width, p.height);
        SDL_SetTextureBlendMode(g_panel, SDL_BLENDMODE_BLEND);
        g_panel_w = p.width;
        g_panel_h = p.height;
    }
    SDL_UpdateTexture(g_panel, nullptr, p.rgba.data(), p.width * 4);
    SDL_Rect dst{p.x, p.y, p.width, p.height};
    SDL_RenderCopy(g_renderer, g_panel, nullptr, &dst);
    SDL_RenderPresent(g_renderer);
}

SDL_HitTestResult hit_test(SDL_Window *, const SDL_Point *at, void *) {
    const quickmenu::Box back = g_menu.back_box();  // (local change: the Back button sits in the title row but is a button)
    const SDL_Rect back_rect{back.x, back.y, back.w, back.h};
    if (back.w > 0 && SDL_PointInRect(at, &back_rect)) return SDL_HITTEST_NORMAL;
    return g_layered && SDL_PointInRect(at, &g_drag) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

void audio_callback(void *, Uint8 *stream, int length) {
    std::fill(stream, stream + length, static_cast<Uint8>(0));
    menusound::mix(reinterpret_cast<std::int16_t *>(stream), static_cast<std::size_t>(length) / 4);
}

// ---- input ----

void handle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_QUIT: g_quit = true; return;
    case SDL_WINDOWEVENT:
        // back to the launcher: the files may have changed meanwhile (the game started from elsewhere, or edited by hand); a Settings
        // page with picks not applied yet keeps what it shows
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && g_menu.change_count() == 0) reload();
        return;
    case SDL_CONTROLLERDEVICEADDED:
        if (SDL_GameController *pad = SDL_GameControllerOpen(e.cdevice.which)) g_pads.push_back(pad);
        return;
    case SDL_CONTROLLERDEVICEREMOVED:
        for (auto it = g_pads.begin(); it != g_pads.end(); ++it)
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(*it)) == e.cdevice.which) {
                SDL_GameControllerClose(*it);
                g_pads.erase(it);
                break;
            }
        return;
    default: break;
    }
    if (g_menu.editing()) {  // naming a card: the computer's keyboard types, the pad works the on-screen keys
        if (e.type == SDL_TEXTINPUT) {
            g_menu.type_text(e.text.text);
            return;
        }
        if (e.type == SDL_KEYDOWN) {
            switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_BACKSPACE: g_menu.erase_text(); return;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                if (!e.key.repeat) {
                    menusound::play(menusound::Clip::select);
                    act(g_menu.finish_edit(true));
                }
                return;
            case SDL_SCANCODE_ESCAPE:
                if (!e.key.repeat) {
                    menusound::play(menusound::Clip::back);
                    g_menu.finish_edit(false);
                }
                return;
            case SDL_SCANCODE_UP: press(Input::up); return;
            case SDL_SCANCODE_DOWN: press(Input::down); return;
            case SDL_SCANCODE_LEFT: press(Input::left); return;
            case SDL_SCANCODE_RIGHT: press(Input::right); return;
            default: return;
            }
        }
    }
    switch (e.type) {
    case SDL_KEYDOWN:
        switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_W: press(Input::up); break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: press(Input::down); break;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: press(Input::left); break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: press(Input::right); break;
        case SDL_SCANCODE_Q: case SDL_SCANCODE_PAGEUP: press(Input::tab_prev); break;
        case SDL_SCANCODE_E: case SDL_SCANCODE_PAGEDOWN: press(Input::tab_next); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
            if (!e.key.repeat) press(Input::confirm);
            break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE:
            if (!e.key.repeat) press(Input::back);
            break;
        default: break;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: press(Input::up); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: press(Input::down); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: press(Input::left); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: press(Input::right); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: press(Input::tab_prev); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: press(Input::tab_next); break;
        case SDL_CONTROLLER_BUTTON_A: press(Input::confirm); break;  // Cross
        case SDL_CONTROLLER_BUTTON_Y: press(Input::back); break;     // Triangle, as in the game's own menus
        default: break;
        }
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY || e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            static std::map<SDL_JoystickID, std::array<int, 2>> was;  // the left stick moves once per push, per controller
            const int axis = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? 1 : 0;
            int &before = was[e.caxis.which][axis];
            const int now = e.caxis.value < -20000 ? -1 : e.caxis.value > 20000 ? 1 : (std::abs(e.caxis.value) < 8000 ? 0 : before);
            if (now && now != before) press(axis ? (now < 0 ? Input::left : Input::right) : (now < 0 ? Input::up : Input::down));
            before = now;
        }
        break;
    case SDL_MOUSEMOTION: {
        const quickmenu::Values v = values();
        g_menu.hover(e.motion.x, e.motion.y, v);
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT) {
            menusound::play(menusound::Clip::select);
            const quickmenu::Values v = values();
            act(g_menu.click(e.button.x, e.button.y, v));
        } else if (e.button.button == SDL_BUTTON_RIGHT) {
            press(Input::back);
        }
        break;
    case SDL_MOUSEWHEEL: press(e.wheel.y > 0 ? Input::up : Input::down); break;
    default: break;
    }
}

bool open_installed(const std::string &config_file) {
    std::string why;
    LaunchConfig c;
    if (!read_launch_config(config_file, c, why)) {
        g_config_problem = why;
        return false;
    }
    g_config = c;
    g_have_config = true;
    g_config_problem.clear();
    g_layout = layout_for(ps2::os::utf8(ps2::os::path(config_file).parent_path()));
    g_mode = installed_version(g_layout).empty() ? Mode::dev : Mode::installed;
    std::fprintf(stderr, "[launcher] settings from %s\n", config_file.c_str());
    const fs::path game = ps2::os::path(under_root(g_config, g_config.game_dir));
    if (fs::is_regular_file(game / "menu" / "menu.ttf")) ps2::os::set_env("PS2_MENU_FONT", ps2::os::utf8(game / "menu" / "menu.ttf"));
    menusound::load(ps2::os::utf8(game / "menu" / "sounds"));
    std::error_code e;
    fs::create_directories(ps2::os::path(under_root(g_config, g_config.card_library)), e);
    reload();
    if (g_mode == Mode::installed) load_state(g_layout, g_install);
    g_menu.show(quickmenu::Page::main);
    return true;
}

}  // namespace

int main(int, char **) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[launcher] SDL: %s\n", SDL_GetError());
        return 1;
    }
    const std::string config_file = find_launch_config(launcher_dir(), ps2::os::utf8(fs::current_path()));
    if (config_file.empty() || !open_installed(config_file)) {
        // no launcher.ini: install (the player's folder is the launcher's own)
        g_mode = Mode::install;
        g_layout = layout_for(launcher_dir());
        g_config_problem.clear();
        load_state(g_layout, g_install);
        if (!g_install.iso.empty()) choose_iso(g_install.iso);
        if (g_install.iso.empty() || !g_iso_line.empty())
            for (const std::string &iso : find_isos(g_layout.base)) {
                choose_iso(iso);
                if (g_iso_line.empty()) break;
            }
    }
    g_webcams = webcam::list();
    // A borderless window that shows only the panel: as high as 90 % of the screen's usable height (at most 960), wide enough for the
    // widest page (Settings); the space around the panel is see-through and lets clicks pass.
    SDL_Rect usable{0, 0, 1280, 800};
    SDL_GetDisplayUsableBounds(0, &usable);
    const int wh = std::min(960, usable.h * 9 / 10), ww = std::min(usable.w, wh * 13 / 10);
    g_window = SDL_CreateWindow("The Sims 2 - Launcher", usable.x + (usable.w - ww) / 2, usable.y + (usable.h - wh) / 2, ww, wh,
                                SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) {
        std::fprintf(stderr, "[launcher] no window: %s\n", SDL_GetError());
        return 1;
    }
    g_layered = make_layered(g_window);
    if (!g_layered) SDL_SetWindowBordered(g_window, SDL_TRUE);
    SDL_SetWindowHitTest(g_window, hit_test, nullptr);
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    g_menu.open();
    if (g_mode == Mode::install) g_menu.show(quickmenu::Page::install);
    if (g_mode == Mode::installed && ps2::os::env("PS2_LAUNCHER_NO_UPDATE_CHECK").empty())
        g_update_check = std::thread([] {  // (never blocks: a failed check finds nothing)
            std::string why;
            const std::string tag = latest_release(kLatestReleaseUrl, why);
            if (newer(tag, installed_version(g_layout))) g_update = tag;
            if (!why.empty()) std::fprintf(stderr, "[launcher] update check: %s\n", why.c_str());
        });
    draw();  // (the first picture before the window shows: no empty frame)
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    SDL_StartTextInput();
    while (!g_quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            handle(e);
            while (!g_quit && SDL_PollEvent(&e)) handle(e);
        }
        install_finished();
        hd_worker_finished();  // local change (stage 3)
        {  // the HD textures' tile while they are missing (an update brings them too: its tile goes first)
            std::lock_guard<std::mutex> hold(g_mod_lock);
            const bool want = g_have_config && g_mode != Mode::install && !g_working.load() && (!g_hd_present || g_mod_busy.load());
            g_menu.set_hd(!want ? std::string() : g_mod_busy.load() ? g_mod_line : "Get the HD textures (" + hdpack::size_text() + ")", g_mod_busy.load());
        }
        if (!g_update.empty() && !g_working.load()) g_menu.set_update(g_update);
        if (!g_quit) draw();
    }
    g_cancel = true;  // (closing during an install stops it; it goes on from that step next time)
    if (g_worker.joinable()) g_worker.join();
    g_mod_cancel = true;  // local change (stage 3): a download stops (unpacking finishes first)
    if (g_mod_worker.joinable()) g_mod_worker.join();
    if (g_update_check.joinable()) g_update_check.join();
    for (SDL_GameController *pad : g_pads) SDL_GameControllerClose(pad);
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
