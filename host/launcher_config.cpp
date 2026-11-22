#include "launcher_config.hpp"

#include "keymap.hpp"
#include "ps2/os.hpp"

#include <cstdio>
#include <filesystem>

namespace {
namespace fs = std::filesystem;

std::string trim(std::string t) {
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
    std::size_t a = 0;
    while (a < t.size() && (t[a] == ' ' || t[a] == '\t')) ++a;
    return t.substr(a);
}

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

bool is_file(const std::string &path) {
    std::error_code e;
    return fs::is_regular_file(ps2::os::path(path), e);
}
}  // namespace

bool read_launch_config(const std::string &file, LaunchConfig &out, std::string &why) {
    std::FILE *f = ps2::os::open(file, "rb");
    if (!f) {
        why = "cannot read " + file;
        return false;
    }
    LaunchConfig c;
    c.root = ps2::os::utf8(ps2::os::path(file).parent_path());
    char line[2048];
    while (std::fgets(line, sizeof line, f)) {
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#') continue;
        const std::size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(text.substr(0, eq)), value = trim(text.substr(eq + 1));
        if (key == "root") c.root = value;
        else if (key == "runner") c.runner = value;
        else if (key == "program") c.program = value;
        else if (key == "iso") c.iso = value;
        else if (key == "game_dir") c.game_dir = value;
        else if (key == "patches") c.patches = value;
        else if (key == "memcard") c.memcard = value;
        else if (key == "card_library") c.card_library = value;
        else if (key == "mods") c.mods = value;
        else if (key == "widescreen") c.widescreen = value != "0" && value != "no" && value != "false";
    }
    std::fclose(f);
    if (!c.root.empty() && !ps2::os::path(c.root).is_absolute())  // a relative root is relative to launcher.ini's folder
        c.root = ps2::os::utf8((ps2::os::path(file).parent_path() / ps2::os::path(c.root)).lexically_normal());
    if (c.iso.empty()) {
        why = file + " has no iso= line (where the game's disc image is)";
        return false;
    }
    out = c;
    return true;
}

std::string find_launch_config(const std::string &exe_dir, const std::string &current_dir) {
    for (const std::string &dir : {exe_dir, current_dir}) {
        if (dir.empty()) continue;
        const std::string file = ps2::os::utf8(ps2::os::path(dir) / "launcher.ini");
        if (is_file(file)) return file;
    }
    return {};
}

std::string under_root(const LaunchConfig &c, const std::string &path) {
    const fs::path p = ps2::os::path(path);
    if (p.is_absolute() || c.root.empty()) return path;
    return ps2::os::utf8((ps2::os::path(c.root) / p).lexically_normal());
}

std::vector<std::string> game_arguments(const LaunchConfig &c) {
    std::vector<std::string> a = {c.program, "--disc", c.iso, "--window"};
    if (c.widescreen) a.push_back("--widescreen");
    for (const std::string &x : {std::string("--game-dir"), c.game_dir, std::string("--patches"), c.patches, std::string("--memcard"), c.memcard,
                                 std::string("--card-library"), c.card_library})
        a.push_back(x);
    return a;
}

std::string windows_command_line(const std::vector<std::string> &args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) out += ' ';
        const std::string &a = args[i];
        if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) {
            out += a;
            continue;
        }
        out += '"';
        std::size_t slashes = 0;
        for (char ch : a) {
            if (ch == '\\') {
                ++slashes;
                continue;
            }
            if (ch == '"') out.append(slashes * 2 + 1, '\\');  // backslashes before a quote are doubled, the quote escaped
            else out.append(slashes, '\\');
            slashes = 0;
            out += ch;
        }
        out.append(slashes * 2, '\\');  // backslashes before the closing quote are doubled
        out += '"';
    }
    return out;
}

std::string swap_new_runner(const LaunchConfig &c) {
    const std::string runner = under_root(c, c.runner);
    const std::string fresh = ps2::os::utf8(ps2::os::path(runner).parent_path() / "ps2run-new.exe");
    if (!is_file(fresh)) return {};
    std::string why;
    if (!ps2::os::move_replacing(fresh, runner, &why)) return "The new build could not be put in place (" + why + "); the game starts with the old one.";
    std::fprintf(stderr, "[launcher] the new build is in place: %s\n", runner.c_str());
    return {};
}

std::string play_blocker(const LaunchConfig &c, bool runner_running, bool runner_exists) {
    if (runner_running) return "The game is already running.";
    if (!runner_exists) return "The game is not built yet (" + c.runner + " is missing).";
    return {};
}

std::string prepare_play(const LaunchConfig &c, bool runner_running, std::string *note) {
    if (runner_running) return play_blocker(c, true, true);
    const std::string swapped = swap_new_runner(c);
    if (note) *note = swapped;
    return play_blocker(c, false, is_file(under_root(c, c.runner)));
}

std::string files_blocker(bool runner_running) {
    return runner_running ? "The game is running and uses these files: close it first, then change cards or settings here." : std::string();
}

LauncherFiles load_files(const LaunchConfig &c) {
    LauncherFiles f;
    const std::string game = under_root(c, c.game_dir);
    f.keys_ini = ps2::os::utf8(ps2::os::path(game) / "keys.ini");
    f.remaster_ini = ps2::os::utf8(ps2::os::path(game) / "remaster.ini");
    f.card_library = under_root(c, c.card_library);
    keymap::Config keys = keymap::defaults();
    std::vector<std::string> problems;
    keymap::parse(read_text(f.keys_ini), keys, problems);
    f.look = keys.look;
    f.scale = keys.scale;
    f.sharp = keys.sharp_2d;
    remaster::read_file_into(f.remaster_ini);
    remaster::aa_from_look(keys.look >= 0 ? keys.look : keymap::kDefaultLook);  // local change: no PS2_AA line yet -> the old smooth-edges bit
    f.extra = remaster::extra_from(remaster::read_file(f.remaster_ini));
    return f;
}
