#pragma once

// Local change (the remaster): the launcher's settings file (launcher.ini), the game's command line, and what Play checks first.
// No window, no processes here (host/launcher_os.cpp starts the game), so all of it can be tested without a game.

#include "remaster_ini.hpp"

#include <string>
#include <vector>

// launcher.ini: "key=value" lines, '#' starts a comment. Paths are relative to `root` (default: the folder of launcher.ini).
struct LaunchConfig {
    std::string root;
    std::string runner = "build\\native-sims2\\ps2run.exe";
    std::string program = "games\\sims2\\patched\\SLES_537.18";
    std::string iso;  // required
    std::string game_dir = "games\\sims2";
    std::string patches = "games\\sims2\\patches.txt";
    std::string memcard = "games\\sims2\\sims2.ps2";
    std::string card_library = "games\\sims2\\cards";
    std::string mods = "games\\sims2\\mods";  // local change (stage 3): the mods folder (host/mod_library.hpp), given to the game as PS2_MODS
    bool widescreen = true;
};

bool read_launch_config(const std::string &file, LaunchConfig &out, std::string &why);
// launcher.ini next to the exe, else in the current folder; "" when neither has one
std::string find_launch_config(const std::string &exe_dir, const std::string &current_dir);
// a path of the config made whole (relative ones are under root)
std::string under_root(const LaunchConfig &c, const std::string &path);

// exactly play-sims2.cmd's arguments, in its order:
// program --disc <iso> --window [--widescreen] --game-dir <dir> --patches <file> --memcard <card> --card-library <dir>
std::vector<std::string> game_arguments(const LaunchConfig &c);
// one Windows command line (UTF-8) from arguments, quoted the way CommandLineToArgvW and the C runtime read it back
std::string windows_command_line(const std::vector<std::string> &args);
// moves <runner's folder>\ps2run-new.exe over the runner when it is there; "" = fine (also when there is nothing to move), else why
std::string swap_new_runner(const LaunchConfig &c);
// why Play cannot start the game now ("" = it can)
std::string play_blocker(const LaunchConfig &c, bool runner_running, bool runner_exists);
// Play's first step: refuses while the game runs (touching nothing), puts a new build (ps2run-new.exe) in place, then checks that a
// game exists. "" = start it; else why not. `note`: a new build that could not be put in place (the old one starts).
std::string prepare_play(const LaunchConfig &c, bool runner_running, std::string *note = nullptr);
// why cards and settings cannot be changed in the launcher now ("" = they can): the running game holds the cards and writes remaster.ini
std::string files_blocker(bool runner_running);

// The files the launcher's pages show, read again whenever the game has ended (the game's quick menu may have changed them).
// remaster.ini fills remaster::g_options as well.
struct LauncherFiles {
    std::string keys_ini, remaster_ini, card_library;  // whole paths
    int look = -1, scale = 0, sharp = -1;                // from keys.ini (-1 / 0 = no line)
    remaster::Extra extra;
};
LauncherFiles load_files(const LaunchConfig &c);
