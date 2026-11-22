// Local change: the launcher's config, the game's command line and Play's decisions (host/launcher_config.cpp), no game needed.
// Build: ninja ps2launchertest, run build/.../ps2launchertest [work folder].
#include "../host/launcher_config.hpp"
#include "../host/remaster_ini.hpp"
#include "ps2/os.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
int failed = 0, checks = 0;
void check(bool ok, const std::string &what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}
void write(const fs::path &p, const std::string &text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}
std::string read(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
fs::path g_work;
int g_case = 0;
fs::path fresh() {
    const fs::path d = g_work / ("case" + std::to_string(++g_case));
    std::error_code e;
    fs::remove_all(d, e);
    fs::create_directories(d);
    return d;
}
const char *kIso = "D:\\PCSX2\\_games\\Sims 2, The (Europe) (En,Fr,De,Es,It,Nl,Sv,No,Da,Fi).iso";
}  // namespace

int main(int argc, char **argv) {
    g_work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ps2launchertest";
    std::error_code error;
    fs::remove_all(g_work, error);
    fs::create_directories(g_work);
    std::string why;
    {  // defaults_and_iso
        const fs::path d = fresh();
        write(d / "launcher.ini", std::string("# my launcher\niso=") + kIso + "\n");
        LaunchConfig c;
        check(read_launch_config(ps2::os::utf8(d / "launcher.ini"), c, why), "defaults_and_iso: read");
        check(c.iso == kIso && c.runner == "build\\native-sims2\\ps2run.exe" && c.game_dir == "games\\sims2" && c.widescreen && c.root == ps2::os::utf8(d),
              "defaults_and_iso: values");
    }
    {  // missing_iso
        const fs::path d = fresh();
        write(d / "launcher.ini", "runner=x.exe\n");
        LaunchConfig c;
        check(!read_launch_config(ps2::os::utf8(d / "launcher.ini"), c, why) && why.find("iso") != std::string::npos, "missing_iso");
    }
    {  // arguments_match_cmd: play-sims2.cmd's argument list as it was on 2026-11-17
        LaunchConfig c;
        c.iso = kIso;
        const std::vector<std::string> want = {"games\\sims2\\patched\\SLES_537.18", "--disc", kIso, "--window", "--widescreen", "--game-dir", "games\\sims2",
                                               "--patches", "games\\sims2\\patches.txt", "--memcard", "games\\sims2\\sims2.ps2", "--card-library", "games\\sims2\\cards"};
        check(game_arguments(c) == want, "arguments_match_cmd");
        c.widescreen = false;
        const std::vector<std::string> narrow = game_arguments(c);
        check(narrow.size() == want.size() - 1 && narrow[4] == "--game-dir", "no_widescreen");
    }
    {  // quoted_paths
        LaunchConfig c;
        c.iso = "E:\\Spiele\\Die Sims 2 \xC3\xABxtra\\game.iso";
        const std::vector<std::string> a = game_arguments(c);
        check(a[2] == c.iso, "quoted_paths: the path is one argument, unchanged");
        check(windows_command_line({"C:\\a b\\x.exe", "plain", "two words", "end\\", "say \"hi\"", ""}) ==
                  "\"C:\\a b\\x.exe\" plain \"two words\" end\\ \"say \\\"hi\\\"\" \"\"",
              "quoted_paths: Windows quoting");
        check(windows_command_line({"x", "dir with space\\"}) == "x \"dir with space\\\\\"", "quoted_paths: a final backslash before the closing quote is doubled");
    }
    {  // swap_new_runner
        const fs::path d = fresh();
        LaunchConfig c;
        c.root = ps2::os::utf8(d);
        c.iso = kIso;
        write(d / "build" / "native-sims2" / "ps2run.exe", "old");
        check(swap_new_runner(c).empty() && read(d / "build" / "native-sims2" / "ps2run.exe") == "old", "swap_new_runner: nothing to swap");
        write(d / "build" / "native-sims2" / "ps2run-new.exe", "new");
        check(swap_new_runner(c).empty() && read(d / "build" / "native-sims2" / "ps2run.exe") == "new" && !fs::exists(d / "build" / "native-sims2" / "ps2run-new.exe"),
              "swap_new_runner: new one in place");
    }
    {  // play_blocker
        LaunchConfig c;
        c.iso = kIso;
        check(play_blocker(c, true, true) == "The game is already running.", "second_play_refused");
        check(play_blocker(c, false, false) == "The game is not built yet (build\\native-sims2\\ps2run.exe is missing).", "missing_runner_message");
        check(play_blocker(c, false, true).empty(), "play_allowed");
        check(files_blocker(true) == "The game is running and uses these files: close it first, then change cards or settings here.", "changes_refused_while_game_runs");
        check(files_blocker(false).empty(), "changes_allowed_without_game");
    }
    {  // prepare_play: the new build is put in place BEFORE checking that a game exists
        const fs::path d = fresh();
        LaunchConfig c;
        c.root = ps2::os::utf8(d);
        c.iso = kIso;
        write(d / "build" / "native-sims2" / "ps2run-new.exe", "new");
        check(prepare_play(c, false).empty() && read(d / "build" / "native-sims2" / "ps2run.exe") == "new", "prepare_play_swaps_first");
        write(d / "build" / "native-sims2" / "ps2run-new.exe", "newer");
        check(prepare_play(c, true) == "The game is already running." && read(d / "build" / "native-sims2" / "ps2run.exe") == "new",
              "prepare_play_running_touches_nothing");
        fs::remove(d / "build" / "native-sims2" / "ps2run-new.exe");
        fs::remove(d / "build" / "native-sims2" / "ps2run.exe");
        check(prepare_play(c, false).find("not built yet") != std::string::npos, "prepare_play_missing");
    }
    {  // config_search: next to the exe first, then the current folder
        const fs::path d = fresh();
        write(d / "exe" / "launcher.ini", "iso=a.iso\n");
        write(d / "cwd" / "launcher.ini", "iso=b.iso\n");
        check(find_launch_config(ps2::os::utf8(d / "exe"), ps2::os::utf8(d / "cwd")) == ps2::os::utf8(d / "exe" / "launcher.ini"), "config_search: beside the exe");
        fs::remove(d / "exe" / "launcher.ini");
        check(find_launch_config(ps2::os::utf8(d / "exe"), ps2::os::utf8(d / "cwd")) == ps2::os::utf8(d / "cwd" / "launcher.ini"), "config_search: the current folder");
        fs::remove(d / "cwd" / "launcher.ini");
        check(find_launch_config(ps2::os::utf8(d / "exe"), ps2::os::utf8(d / "cwd")).empty(), "config_search: none");
    }
    {  // reload_after_game: the files as they are now on disk
        const fs::path d = fresh();
        LaunchConfig c;
        c.root = ps2::os::utf8(d);
        c.iso = kIso;
        write(d / "games" / "sims2" / "keys.ini", "look = soft\nscale = 2\n");
        write(d / "games" / "sims2" / "remaster.ini", "PS2_REFRESH_HZ=60\nPS2_CARD1=Twinbrook\nPS2_CARD2=none\n");
        LauncherFiles f = load_files(c);
        const bool first = f.scale == 2 && remaster::g_options[0].next == 60 && f.extra.card[0] == "Twinbrook";
        write(d / "games" / "sims2" / "keys.ini", "look = sharp\nscale = 3\n");
        write(d / "games" / "sims2" / "remaster.ini", "PS2_REFRESH_HZ=50\nPS2_CARD1=Riverview\nPS2_CARD2=none\n");
        f = load_files(c);
        check(first && f.scale == 3 && remaster::g_options[0].next == 50 && f.extra.card[0] == "Riverview", "reload_after_game");
        const auto same = [](const std::string &a, const fs::path &b) { return ps2::os::path(a).lexically_normal() == b.lexically_normal(); };
        check(same(f.remaster_ini, d / "games" / "sims2" / "remaster.ini") && same(f.card_library, d / "games" / "sims2" / "cards"),
              "reload_after_game: paths under root");
    }
    std::printf("launcher: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    if (!failed) fs::remove_all(g_work, error);
    return failed ? 1 : 0;
}
