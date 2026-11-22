// Local change: the Memory cards page's actions (host/card_page.cpp) with the launcher's file slots, no game needed.
// Build: ninja ps2cardpagetest, run build/.../ps2cardpagetest [work folder].
#include "../host/card_page.hpp"
#include "../host/card_library.hpp"
#include "ps2/os.hpp"

#include <cstdio>
#include <filesystem>
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
fs::path g_work;
int g_case = 0;
std::string fresh() {
    const fs::path d = g_work / ("case" + std::to_string(++g_case));
    std::error_code e;
    fs::remove_all(d, e);
    fs::create_directories(d);
    return ps2::os::utf8(d);
}
int row_of(const quickmenu::Values &v, const std::string &name) {
    for (std::size_t i = 0; i < v.cards.size(); ++i)
        if (v.cards[i].name == name) return static_cast<int>(i);
    return -1;
}
using quickmenu::Command;
}  // namespace

int main(int argc, char **argv) {
    g_work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ps2cardpagetest";
    std::error_code error;
    fs::remove_all(g_work, error);
    fs::create_directories(g_work);
    std::string why;
    {
        const std::string dir = fresh();
        std::string names[2];
        int remembered = 0;
        const cardpage::Slots slots = cardpage::file_slots(names, [&] { ++remembered; });
        const std::string line = cardpage::act({Command::card_new}, "", dir, slots);
        quickmenu::Values v;
        cardpage::fill(v, dir, slots);
        check(v.card_library && v.cards.size() == 1 && line.find(v.cards[0].name) != std::string::npos, "new_card");

        cardlib::create(dir, "Twinbrook", why);
        cardpage::fill(v = {}, dir, slots);
        cardpage::act({Command::card_insert, row_of(v, "Twinbrook"), 2}, "", dir, slots);
        check(names[1] == "Twinbrook" && remembered == 1, "insert_and_remember");
        cardpage::fill(v = {}, dir, slots);
        check(v.slot_cards[1] == "Twinbrook" && v.cards[row_of(v, "Twinbrook")].slot == 2, "insert_and_remember: shown in slot 2");

        const std::string refused = cardpage::act({Command::card_insert, row_of(v, "Twinbrook"), 1}, "", dir, slots);
        check(names[0].empty() && refused.find("slot 2") != std::string::npos, "refuse_other_slot");

        cardpage::act({Command::card_rename, row_of(v, "Twinbrook")}, "Bridgeport", dir, slots);
        check(names[1] == "Bridgeport" && cardlib::exists(dir, "Bridgeport") && !cardlib::exists(dir, "Twinbrook"), "rename_in_slot_updates_name");

        cardpage::fill(v = {}, dir, slots);
        const std::string del = cardpage::act({Command::card_delete, row_of(v, "Bridgeport")}, "", dir, slots);
        check(cardlib::exists(dir, "Bridgeport") && del.find("out") != std::string::npos, "delete_refused_in_slot");

        cardpage::act({Command::card_eject, 2}, "", dir, slots);
        check(names[1].empty(), "eject_empties_the_slot");

        cardlib::create(dir, "Riverview", why);
        cardpage::fill(v = {}, dir, slots);
        cardpage::act({Command::card_copy, row_of(v, "Riverview")}, "", dir, slots);
        check(cardlib::exists(dir, "Riverview 2"), "copy_unique_name");
    }
    {
        const std::string dir = fresh();
        std::string names[2];
        const cardpage::Slots slots = cardpage::file_slots(names, [] {});
        check(cardlib::create(dir, "Zo\xC3\xAB", why), "utf8_card_names: created");
        quickmenu::Values v;
        cardpage::fill(v, dir, slots);
        cardpage::act({Command::card_insert, row_of(v, "Zo\xC3\xAB"), 1}, "", dir, slots);
        cardpage::act({Command::card_rename, row_of(v, "Zo\xC3\xAB")}, "Zo\xC3\xAB's story", dir, slots);
        cardpage::fill(v = {}, dir, slots);
        check(names[0] == "Zo\xC3\xAB's story" && row_of(v, "Zo\xC3\xAB's story") >= 0 && fs::exists(ps2::os::path(cardlib::file_of(dir, "Zo\xC3\xAB's story"))),
              "utf8_card_names");
    }
    std::printf("card page: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    if (!failed) fs::remove_all(g_work, error);
    return failed ? 1 : 0;
}
