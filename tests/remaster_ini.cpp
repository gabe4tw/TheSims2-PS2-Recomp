// Local change: remaster.ini (host/remaster_ini.cpp), no game needed. Build: ninja ps2remasterinitest, run build/.../ps2remasterinitest [work folder].
#include "../host/remaster_ini.hpp"
#include "ps2/os.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
    std::ofstream out(p, std::ios::binary);
    out << text;
}
std::string read(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
int count(const std::string &text, const std::string &what) {
    int n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}
void reset_options() {
    for (int i = 0; i < remaster::kOptionCount; ++i) remaster::g_options[i].now = remaster::g_options[i].next = remaster::g_options[i].fallback;
}
}  // namespace

int main(int argc, char **argv) {
    const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ps2remasterinitest";
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(work);
    const std::string ini = ps2::os::utf8(work / "remaster.ini");

    {  // round_trip
        reset_options();
        remaster::g_options[0].next = 60;
        remaster::Extra e;
        e.window_mode = 1;
        e.player[0] = "all";
        e.player[1] = "none";
        e.webcam = "test";
        e.screen_res = "1920x1080";
        e.card[0] = "Pleasantview";
        e.card[1] = "none";
        e.card_library = true;
        check(remaster::write(ini, e), "round_trip: written");
        const auto m = remaster::read_file(ini);
        const auto is = [&](const char *k, const char *v) { return m.count(k) && m.at(k) == v; };
        check(is("PS2_REFRESH_HZ", "60") && is("PS2_WINDOW_MODE", "1") && is("PS2_PLAYER1", "all") && is("PS2_PLAYER2", "none") && is("PS2_WEBCAM", "test") &&
                  is("PS2_SCREEN_RES", "1920x1080") && is("PS2_CARD1", "Pleasantview") && is("PS2_CARD2", "none") && is("PS2_SIMS2_TICKFIX", "1"),
              "round_trip: every line read back");
        const remaster::Extra back = remaster::extra_from(m);
        check(back.window_mode == 1 && back.player[0] == "all" && back.webcam == "test" && back.screen_res == "1920x1080" && back.card[0] == "Pleasantview" &&
                  back.card_library,
              "round_trip: extra_from");
    }
    {  // keep_next_survives_launcher: the Debug page's one-shot line must survive a launcher write
        reset_options();
        std::ofstream(work / "remaster.ini", std::ios::binary) << "PS2_GAME_POKES_NEXT=5186DC:1:FF:1:0\nPS2_CARD1=A\n";
        remaster::Extra e = remaster::extra_from(remaster::read_file(ini));
        remaster::write(ini, e);
        check(count(read(work / "remaster.ini"), "PS2_GAME_POKES_NEXT=5186DC:1:FF:1:0") == 1, "keep_next_survives_launcher");
    }
    {  // first_start_cards_recorded: the game's first-start card choice goes into the file when it has no card lines yet
        reset_options();
        std::ofstream(work / "remaster.ini", std::ios::binary) << "PS2_REFRESH_HZ=60\nPS2_FUTURE_THING=7\n";
        check(remaster::add_card_lines_if_missing(ini, "Copperdale", ""), "first_start_cards_recorded: written");
        const auto m = remaster::read_file(ini);
        check(m.count("PS2_CARD1") && m.at("PS2_CARD1") == "Copperdale" && m.at("PS2_CARD2") == "none" && m.at("PS2_REFRESH_HZ") == "60" && m.count("PS2_FUTURE_THING"),
              "first_start_cards_recorded: cards in, the rest kept");
        check(!remaster::add_card_lines_if_missing(ini, "Other", "X") && remaster::read_file(ini).at("PS2_CARD1") == "Copperdale", "existing_card_lines_left_alone");
        std::remove(ini.c_str());
        check(remaster::add_card_lines_if_missing(ini, "A", "B") && remaster::read_file(ini).at("PS2_CARD2") == "B", "first_start_no_file_yet");
    }
    {  // unknown_lines_kept
        reset_options();
        write(work / "remaster.ini", "# a comment\nPS2_FUTURE_THING=7\nPS2_REFRESH_HZ=90\nPS2_GAME_POKES=1:2:3:4:5\nPS2_OTHER=a b\n");
        check(remaster::write(ini, remaster::Extra{}), "unknown_lines_kept: written");
        const std::string text = read(work / "remaster.ini");
        const std::size_t a = text.find("PS2_FUTURE_THING=7"), b = text.find("PS2_OTHER=a b");
        check(count(text, "PS2_FUTURE_THING=7") == 1 && count(text, "PS2_OTHER=a b") == 1 && a < b, "unknown_lines_kept: both kept once, in order");
        check(count(text, "PS2_GAME_POKES=") == 0, "unknown_lines_kept: an old cheat line (PS2_GAME_POKES) is dropped, cheats never last");
    }
    {  // owned_lines_not_doubled
        reset_options();
        write(work / "remaster.ini", "PS2_REFRESH_HZ=90\nPS2_WINDOW_MODE=2\nPS2_CARD1=Old\n");
        remaster::g_options[0].next = 60;
        remaster::write(ini, remaster::Extra{});
        const std::string text = read(work / "remaster.ini");
        check(count(text, "PS2_REFRESH_HZ=") == 1 && count(text, "PS2_REFRESH_HZ=60") == 1 && count(text, "PS2_WINDOW_MODE=") == 1, "owned_lines_not_doubled");
        check(count(text, "PS2_CARD1=") == 0, "owned_lines_not_doubled: a card line is not kept when the library is off");
    }
    {  // empty_extra_lines_skipped
        reset_options();
        write(work / "remaster.ini", "");
        remaster::write(ini, remaster::Extra{});
        const std::string text = read(work / "remaster.ini");
        check(count(text, "PS2_SCREEN_RES") == 0 && count(text, "PS2_CARD1") == 0 && count(text, "PS2_PADMAP") == 0 && count(text, "PS2_GAME_POKES_NEXT") == 0,
              "empty_extra_lines_skipped");
    }
    {  // read_file_into
        reset_options();
        write(work / "remaster.ini", "PS2_ASPECT=219\n");
        remaster::read_file_into(ini);
        check(remaster::g_options[9].next == 219 && remaster::g_options[9].now == 219, "read_file_into: value");
        check(remaster::g_options[0].next == remaster::g_options[0].fallback, "read_file_into: missing variable -> fallback");
        write(work / "remaster.ini", "PS2_UI_SQUEEZE=1\n");
        remaster::read_file_into(ini);
        check(remaster::g_options[1].next == 75, "read_file_into: PS2_UI_SQUEEZE=1 is the true shape (75)");
    }
    {  // local change: PS2_AA is read, noted as given, and written back
        reset_options();
        write(work / "remaster.ini", "PS2_ASPECT=219\n");
        remaster::read_file_into(ini);
        const bool missing = !remaster::g_aa_given;
        write(work / "remaster.ini", "PS2_AA=3\n");
        remaster::read_file_into(ini);
        check(missing && remaster::g_aa_given && remaster::g_options[remaster::kAaOption].next == 3, "aa_read");
        remaster::g_options[remaster::kAaOption].next = 0;
        remaster::write(ini, remaster::Extra{});
        check(count(read(work / "remaster.ini"), "PS2_AA=0") == 1, "aa_written");
    }
    {  // mod_owned_lines_kept (launcher stage 3): a mod's setting never goes into the file
        reset_options();
        write(work / "remaster.ini", "PS2_DEBAND=1\nPS2_OTHER=x\n");
        remaster::g_mod_owned = {{"PS2_DEBAND", "Mod A"}, {"PS2_REFRESH_HZ", "Mod B"}};
        remaster::g_options[5].next = 2;   // the mod's value this run
        remaster::g_options[0].next = 120;
        remaster::write(ini, remaster::Extra{});
        const std::string text = read(work / "remaster.ini");
        check(count(text, "PS2_DEBAND=1") == 1 && count(text, "PS2_DEBAND=") == 1, "mod_owned_lines_kept: the file's own line stays");
        check(count(text, "PS2_REFRESH_HZ=") == 0, "mod_owned_lines_kept: no line when the file had none");
        check(count(text, "PS2_OTHER=x") == 1, "mod_owned_lines_kept: other lines stay");
        check(remaster::mod_owner(5) == "Mod A" && remaster::mod_owner(6).empty(), "mod_owner");
        remaster::g_mod_owned.clear();
    }
    std::printf("remaster ini: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    if (!failed) fs::remove_all(work, error);
    return failed ? 1 : 0;
}
