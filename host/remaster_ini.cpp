#include "remaster_ini.hpp"

#include "ps2/os.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>

namespace remaster {

Option g_options[kOptionCount] = {
    {"PS2_REFRESH_HZ", {50, 60, 75, 90, 100, 120, 144, 165, 240}, 50},
    {"PS2_UI_SQUEEZE", {75, 80, 85, 90, 100}, 100},
    {"PS2_LOAD_TURBO", {0, 1}, 0},
    {"PS2_DISC_SPEED", {1, 2, 4, 8, 16}, 1},
    {"PS2_PAD_MERGE", {0, 1}, 0},
    {"PS2_DEBAND", {0, 1, 2}, 0},         // smoothing colour bands in the present shader (blit.wgsl): off, light, strong; used at once
    {"PS2_TEX_ENHANCED", {0, 1}, 1},      // the larger textures of the game folder's textures/enhanced, or the game's own; used at once
    {"PS2_VSYNC", {0, 1}, 0},             // present in step with the screen's refresh (WebGPU Fifo) instead of Mailbox; next start
    {"PS2_SKIP_INTRO", {0, 1}, 0},        // the game's own skip_startup_movies (runtime/src/system.cpp); next start
    {"PS2_ASPECT", {43, 169, 219}, 169},  // the picture's shape: 4:3, 16:9, 21:9 (main.cpp, system.cpp); next start
    {"PS2_AA", {0, 1, 3, 4, 5, 6, 7, 8, 9}, 1},  // anti-aliasing: Off, Fast (before the present), then supersampling 2x .. 8x = value - 1
                                                 // (gpu.cpp); 2 = Quality, kept free (experimental, PS2_AA=2 by hand only); see needs_restart
    {"PS2_MOUSE_MODE", {1, 0}, 1},  // local change: 1 = the PC mouse (host/mouse_pc.cpp), 0 = keys.ini's mouse lines (as a controller); at once
};
bool g_aa_given = false;
void aa_from_look(int look) {
    if (g_aa_given || look < 0) return;
    g_options[kAaOption].now = g_options[kAaOption].next = look % 2;
}

std::map<std::string, std::string> g_mod_owned;
std::string mod_owner(int i) {
    const auto it = g_mod_owned.find(g_options[i].variable);
    return it == g_mod_owned.end() ? std::string() : it->second;
}

bool needs_restart(int i) {
    if (i == kAaOption) return g_options[i].now >= 2 || g_options[i].next >= 2;
    if (i == kMouseOption) return false;
    return i <= 4 || i >= 7;
}

std::string word(int index, int value) {
    switch (index) {
    case 0: return std::to_string(value) + " fps" + (value == 50 ? " (original)" : "");
    case 1: return value == 75 ? "true shape (75 %)" : value == 100 ? "stretched (100 %)" : std::to_string(value) + " % wide";
    case 2: return value ? "on" : "off";
    case 3: return std::to_string(value) + "x" + (value == 1 ? " (original)" : "");
    case 5: return value == 0 ? "off" : value == 1 ? "light" : "strong";
    case 6: return value ? "updated (HD)" : "original";
    case 7: case 8: return value ? "on" : "off";
    case 9: return value == 43 ? "4:3" : value == 219 ? "21:9" : "16:9";
    case 11: return value ? "PC style" : "As a controller";
    case 10: return value == 0 ? "Off" : value == 1 ? "Fast" : value == 2 ? "Quality" : std::to_string(value - 1) + "x";
    default: return value ? "all control player 1" : "first two are players 1 and 2";
    }
}

namespace {
void set_from_text(int i, const std::string &text) {
    Option &o = g_options[i];
    int value = text.empty() ? o.fallback : std::atoi(text.c_str());
    if (i == 1 && value == 1) value = 75;  // PS2_UI_SQUEEZE=1 is the true shape
    o.now = o.next = value;
}

// every line name this module writes (or drops)
const std::set<std::string> &owned() {
    static const std::set<std::string> names = [] {
        std::set<std::string> n = {"PS2_SIMS2_TICKFIX", "PS2_WINDOW_MODE", "PS2_PLAYER1", "PS2_PLAYER2", "PS2_WEBCAM", "PS2_SCREEN_RES", "PS2_PADMAP",
                                   "PS2_GAME_POKES_NEXT", "PS2_GAME_POKES", "PS2_CARD1", "PS2_CARD2"};
        for (const Option &o : g_options) n.insert(o.variable);
        return n;
    }();
    return names;
}

// the PS2_ lines of a file as they are, in order (name, whole line)
std::vector<std::pair<std::string, std::string>> file_lines(const std::string &path) {
    std::vector<std::pair<std::string, std::string>> out;
    if (std::FILE *f = ps2::os::open(path, "rb")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::string text = line;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
            const std::size_t eq = text.find('=');
            if (text.rfind("PS2_", 0) != 0 || eq == std::string::npos) continue;
            out.emplace_back(text.substr(0, eq), text);
        }
        std::fclose(f);
    }
    return out;
}
}  // namespace

void read_env() {
    for (int i = 0; i < kOptionCount; ++i) set_from_text(i, ps2::os::env(g_options[i].variable));
    g_aa_given = !ps2::os::env(g_options[kAaOption].variable).empty();
}

std::map<std::string, std::string> read_file(const std::string &path) {
    std::map<std::string, std::string> out;
    for (const auto &[name, line] : file_lines(path)) out[name] = line.substr(name.size() + 1);
    return out;
}

void read_file_into(const std::string &path) {
    const auto lines = read_file(path);
    for (int i = 0; i < kOptionCount; ++i) {
        const auto it = lines.find(g_options[i].variable);
        set_from_text(i, it == lines.end() ? std::string() : it->second);
    }
    const auto aa = lines.find(g_options[kAaOption].variable);
    g_aa_given = aa != lines.end() && !aa->second.empty();
}

Extra extra_from(const std::map<std::string, std::string> &lines) {
    Extra e;
    const auto get = [&](const char *name, const std::string &fallback) {
        const auto it = lines.find(name);
        return it == lines.end() || it->second.empty() ? fallback : it->second;
    };
    e.window_mode = std::atoi(get("PS2_WINDOW_MODE", "0").c_str());
    e.player[0] = get("PS2_PLAYER1", "auto");
    e.player[1] = get("PS2_PLAYER2", "auto");
    if (e.player[0] == "auto" && get("PS2_PAD_MERGE", "0") == "1") e.player[0] = "all";  // (an old file)
    e.webcam = get("PS2_WEBCAM", "none");
    e.screen_res = get("PS2_SCREEN_RES", "");
    e.padmap = get("PS2_PADMAP", "");
    e.pokes_next = get("PS2_GAME_POKES_NEXT", "");  // (the Debug page's one-shot line is kept until the game has used it)
    e.card_library = lines.count("PS2_CARD1") || lines.count("PS2_CARD2");
    e.card[0] = get("PS2_CARD1", "");
    e.card[1] = get("PS2_CARD2", "");
    return e;
}

bool add_card_lines_if_missing(const std::string &path, const std::string &card1, const std::string &card2) {
    if (path.empty()) return false;
    for (const auto &[name, line] : file_lines(path))
        if (name == "PS2_CARD1" || name == "PS2_CARD2") return false;
    std::string text;
    if (std::FILE *f = ps2::os::open(path, "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    if (!text.empty() && text.back() != '\n') text += "\n";
    text += "PS2_CARD1=" + (card1.empty() ? std::string("none") : card1) + "\nPS2_CARD2=" + (card2.empty() ? std::string("none") : card2) + "\n";
    return ps2::os::replace_file(path, text.data(), text.size());
}

bool write(const std::string &path, const Extra &e) {
    if (path.empty()) return false;
    std::string text = "# Options of the quick menu (F1, or both sticks pressed in, L3 + R3, on a controller) and of the launcher. Used when the game starts.\n"
                       "# A variable that the launcher sets itself wins over the line here.\n";
    const auto old_lines = file_lines(path);
    for (const Option &o : g_options) {
        if (g_mod_owned.count(o.variable)) {  // a mod's value: the file keeps its own line (or none)
            for (const auto &[name, line] : old_lines)
                if (name == o.variable) text += line + "\n";
            continue;
        }
        text += std::string(o.variable) + "=" + std::to_string(o.next) + "\n";
    }
    text += "PS2_SIMS2_TICKFIX=1\n";  // the frame rate needs the game's own clock corrected (runtime/src/system.cpp)
    text += "PS2_WINDOW_MODE=" + std::to_string(e.window_mode) + "\n";
    text += "PS2_PLAYER1=" + e.player[0] + "\nPS2_PLAYER2=" + e.player[1] + "\n";  // Settings > Controls > Players
    text += "PS2_WEBCAM=" + e.webcam + "\n";                                       // Settings > Controls > EyeToy camera
    if (!e.pokes_next.empty()) text += "PS2_GAME_POKES_NEXT=" + e.pokes_next + "\n";  // the Debug page's one-shot "keep for the next start"
    if (!e.screen_res.empty()) text += "PS2_SCREEN_RES=" + e.screen_res + "\n";
    if (e.card_library) text += "PS2_CARD1=" + (e.card[0].empty() ? std::string("none") : e.card[0]) + "\nPS2_CARD2=" + (e.card[1].empty() ? std::string("none") : e.card[1]) + "\n";
    if (!e.padmap.empty()) text += "PS2_PADMAP=" + e.padmap + "\n";  // "Change controller buttons"
    for (const auto &[name, line] : old_lines)
        if (!owned().count(name)) text += line + "\n";  // lines of a newer runner or written by hand stay
    std::string why;
    if (!ps2::os::replace_file(path, text.data(), text.size(), &why)) {
        std::fprintf(stderr, "[options] cannot write %s (%s)\n", path.c_str(), why.c_str());
        return false;
    }
    return true;
}

}  // namespace remaster
