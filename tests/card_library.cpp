// Local change: the memory card library (host/card_library.cpp), no game needed. Build: ninja ps2cardlibtest, run
// build/.../ps2cardlibtest [work folder] (prints "card library: N of N checks passed, 0 failed").
#include "../host/card_library.hpp"
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
}  // namespace

int main(int argc, char **argv) {
    g_work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ps2cardlibtest";
    std::error_code error;
    fs::remove_all(g_work, error);
    fs::create_directories(g_work);
    using namespace cardlib;

    // names
    check(clean_name("  Pleasant:view?  ") == "Pleasantview", "clean_name drops characters a file name cannot have and the spaces at the ends");
    check(clean_name("a.fresh").empty() && clean_name("x.crash").empty(), "clean_name refuses the runtime's own file names");
    check(clean_name("CON").empty(), "clean_name refuses a reserved Windows name");
    check(clean_name(std::string(40, 'x')).size() == static_cast<std::size_t>(kNameMax), "clean_name keeps at most kNameMax characters");
    check(clean_name("Town.") == "Town", "clean_name drops a final dot");
    check(unique_name("Riverview", {"riverview", "Riverview 2"}) == "Riverview 3", "unique_name counts on past names taken, without regard to case");
    {
        std::vector<std::string> taken;
        for (int i = 0; i < 40; ++i) taken.push_back(random_name(taken));
        bool distinct = true;
        for (std::size_t i = 0; i < taken.size(); ++i)
            for (std::size_t j = i + 1; j < taken.size(); ++j) distinct = distinct && taken[i] != taken[j];
        check(distinct, "random_name never repeats a name that is taken (also after all towns are used)");
    }

    // the list leaves out the runtime's files
    {
        const fs::path d = fresh();
        const std::string dir = ps2::os::utf8(d);
        write(d / "Twinbrook.ps2", "x");
        write(d / "Twinbrook.fresh.ps2", "x");
        write(d / "Twinbrook.crash.ps2", "x");
        write(d / "Twinbrook.damaged-2026-11-17_01-00-00.ps2", "x");
        write(d / "Twinbrook.ps2.lock", "");
        write(d / "notes.txt", "x");
        fs::create_directories(d / "Twinbrook.backups");
        write(d / "Bridgeport.PS2", "y");
        const std::vector<std::string> l = list(dir);
        check(l.size() == 2 && l[0] == "Bridgeport" && l[1] == "Twinbrook", "list: only the cards, sorted");
    }

    // create / copy / rename / remove
    {
        const fs::path d = fresh();
        const std::string dir = ps2::os::utf8(d);
        std::string why;
        check(create(dir, "Oasis Springs", why) && fs::file_size(d / "Oasis Springs.ps2") == 0, "create: an empty card file");
        check(!create(dir, "oasis springs", why), "create: refuses a name that is there (any case)");
        check(when(dir, "Oasis Springs").empty(), "when: a new card has never been written");
        write(d / "Oasis Springs.ps2", "card data");
        check(!when(dir, "Oasis Springs").empty(), "when: a written card has a time");
        check(copy(dir, "Oasis Springs", "Oasis Springs 2", why) && read(d / "Oasis Springs 2.ps2") == "card data", "copy: the same bytes under the new name");
        fs::create_directories(d / "Oasis Springs.backups");
        write(d / "Oasis Springs.backups" / "Oasis Springs_2026-11-17_01-00-00.ps2", "old");
        check(rename(dir, "Oasis Springs", "Windenburg", why), "rename: done");
        check(!fs::exists(d / "Oasis Springs.ps2") && read(d / "Windenburg.ps2") == "card data", "rename: the file has the new name");
        check(read(d / "Windenburg.backups" / "Windenburg_2026-11-17_01-00-00.ps2") == "old", "rename: the backups folder and its copies follow");
        check(!rename(dir, "Windenburg", "Oasis Springs 2", why), "rename: refuses the name of another card");
        write(d / "Windenburg.crash.ps2", "c");
        write(d / "Windenburg.damaged-2026-11-17_01-00-00.ps2", "d");
        check(remove(dir, "Windenburg", why), "remove: done");
        check(!fs::exists(d / "Windenburg.ps2") && !fs::exists(d / "Windenburg.backups") && !fs::exists(d / "Windenburg.crash.ps2") &&
                  !fs::exists(d / "Windenburg.damaged-2026-11-17_01-00-00.ps2"),
              "remove: the card, its backups and the runtime's files beside it are gone");
        check(fs::exists(d / "Oasis Springs 2.ps2"), "remove: other cards stay");
    }

    // the start
    {
        const fs::path d = fresh();
        const std::string dir = ps2::os::utf8(d / "cards");
        write(d / "old.ps2", "the player's card");
        const Start s = prepare(dir, ps2::os::utf8(d / "old.ps2"), "", "");
        check(!s.slot[0].empty() && !s.slot[1].empty() && s.slot[0] != s.slot[1], "first start: a card in each slot");
        check(read(fs::path(file_of(dir, s.slot[0]))) == "the player's card", "first start: slot 1 holds a copy of the launcher's card");
        check(read(d / "old.ps2") == "the player's card", "first start: the launcher's card file stays as it was");
        check(fs::file_size(fs::path(file_of(dir, s.slot[1]))) == 0, "first start: slot 2 holds a new empty card");
        const Start again = prepare(dir, ps2::os::utf8(d / "old.ps2"), s.slot[1], "none");
        check(again.slot[0] == s.slot[1] && again.slot[1].empty(), "later start: the names from remaster.ini; \"none\" leaves the slot empty");
        check(list(dir).size() == 2, "later start: no new cards");
        const Start gone = prepare(dir, "", "Not There", s.slot[0]);
        check(gone.slot[0].empty() && gone.slot[1] == s.slot[0], "a card that is gone leaves its slot empty");
        const Start same = prepare(dir, "", s.slot[0], s.slot[0]);
        check(same.slot[0] == s.slot[0] && same.slot[1].empty(), "one card in both slots: slot 2 stays empty");
    }
    {
        const fs::path d = fresh();
        const std::string dir = ps2::os::utf8(d / "cards");
        const Start s = prepare(dir, ps2::os::utf8(d / "missing.ps2"), "", "");
        check(!s.slot[0].empty() && !s.slot[1].empty() && list(dir).size() == 2, "first start without a launcher card: two new cards");
    }

    std::printf("card library: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    if (!failed) fs::remove_all(g_work, error);
    return failed ? 1 : 0;
}
