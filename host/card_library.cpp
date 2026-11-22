#include "card_library.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <random>
#include <system_error>

namespace cardlib {

namespace {
namespace fs = std::filesystem;

// towns and neighbourhoods of The Sims 1 to 4 (the user's list, NOTES.md 7b)
constexpr const char *kTowns[] = {
    "Downtown", "Vacation Island", "Old Town", "Studio Town", "Magic Town",
    "Pleasantview", "Strangetown", "Veronaville", "Riverview", "Desiderata Valley", "Belladonna Cove", "Bluewater Village", "Riverblossom Hills",
    "Sunset Valley", "Twinbrook", "Bridgeport", "Appaloosa Plains", "Moonlight Falls", "Isla Paradiso", "Monte Vista",
    "Willow Creek", "Oasis Springs", "Newcrest", "Magnolia Promenade", "Windenburg", "San Myshuno", "Brindleton Bay", "Del Sol Valley",
    "StrangerVille", "Glimmerbrook", "Forgotten Hollow", "Henford-on-Bagley", "Copperdale", "Tomarang",
};

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string &s, const std::string &tail) { return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0; }

// a name the runtime gives to its own files beside a card (sio2.cpp: beside(card, "fresh") etc.)
bool runtime_file_stem(const std::string &stem) {
    const std::string l = lower(stem);
    return ends_with(l, ".fresh") || ends_with(l, ".crash") || l.find(".damaged-") != std::string::npos;
}

bool taken_by(const std::vector<std::string> &taken, const std::string &name) {
    const std::string l = lower(name);
    return std::any_of(taken.begin(), taken.end(), [&](const std::string &t) { return lower(t) == l; });
}

fs::path backups_of(const std::string &dir, const std::string &name) { return ps2::os::path(dir) / ps2::os::path(name + ".backups"); }

}  // namespace

std::vector<std::string> list(const std::string &dir) {
    std::vector<std::string> out;
    std::error_code error;
    for (fs::directory_iterator it(ps2::os::path(dir), error), end; !error && it != end; it.increment(error)) {
        std::error_code e;
        if (!it->is_regular_file(e)) continue;
        const fs::path p = it->path();
        if (lower(ps2::os::utf8(p.extension())) != ".ps2") continue;
        const std::string stem = ps2::os::utf8(p.stem());
        if (stem.empty() || runtime_file_stem(stem)) continue;
        out.push_back(stem);
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
    return out;
}

std::string file_of(const std::string &dir, const std::string &name) { return ps2::os::utf8(ps2::os::path(dir) / ps2::os::path(name + ".ps2")); }

std::string clean_name(const std::string &name) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos) continue;
        out += static_cast<char>(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();  // (Windows drops a final dot or space)
    std::size_t start = 0;
    while (start < out.size() && out[start] == ' ') ++start;
    out.erase(0, start);
    // at most kNameMax code points
    std::size_t points = 0, cut = out.size();
    for (std::size_t i = 0; i < out.size(); ++i) {
        if ((static_cast<unsigned char>(out[i]) & 0xC0) == 0x80) continue;
        if (points == static_cast<std::size_t>(kNameMax)) {
            cut = i;
            break;
        }
        ++points;
    }
    out.erase(cut);
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (runtime_file_stem(out)) return {};
    static const char *const kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "lpt1", "lpt2", "lpt3"};
    for (const char *r : kReserved)
        if (lower(out) == r) return {};
    return out;
}

std::string unique_name(const std::string &base, const std::vector<std::string> &taken) {
    if (!taken_by(taken, base)) return base;
    for (int n = 2; n < 1000; ++n) {
        const std::string candidate = base + " " + std::to_string(n);
        if (!taken_by(taken, candidate)) return candidate;
    }
    return base + " " + std::to_string(std::random_device{}());
}

std::string random_name(const std::vector<std::string> &taken) {
    std::vector<std::string> free;
    for (const char *t : kTowns)
        if (!taken_by(taken, t)) free.push_back(t);
    std::mt19937 rng(std::random_device{}());
    if (!free.empty()) return free[std::uniform_int_distribution<std::size_t>(0, free.size() - 1)(rng)];
    const std::size_t n = std::size(kTowns);
    return unique_name(kTowns[std::uniform_int_distribution<std::size_t>(0, n - 1)(rng)], taken);
}

bool exists(const std::string &dir, const std::string &name) { return taken_by(list(dir), name); }

bool create(const std::string &dir, const std::string &name, std::string &why) {
    if (name.empty()) {
        why = "no name";
        return false;
    }
    if (exists(dir, name)) {
        why = "a card called \"" + name + "\" is there already";
        return false;
    }
    std::error_code error;
    fs::create_directories(ps2::os::path(dir), error);
    return ps2::os::replace_file(file_of(dir, name), "", 0, &why);
}

bool copy(const std::string &dir, const std::string &from, const std::string &to, std::string &why) {
    if (to.empty() || exists(dir, to)) {
        why = to.empty() ? "no name" : "a card called \"" + to + "\" is there already";
        return false;
    }
    std::error_code error;
    fs::copy_file(ps2::os::path(file_of(dir, from)), ps2::os::path(file_of(dir, to)), fs::copy_options::none, error);
    if (error) {
        why = error.message();
        return false;
    }
    return true;
}

bool remove(const std::string &dir, const std::string &name, std::string &why) {
    std::error_code error;
    const fs::path card = ps2::os::path(file_of(dir, name));
    if (!fs::remove(card, error) || error) {
        why = error ? error.message() : "the card file is not there";
        return false;
    }
    fs::remove_all(backups_of(dir, name), error);
    const fs::path folder = ps2::os::path(dir);
    std::vector<fs::path> beside;
    std::error_code list_error;
    const std::string l = lower(name);
    for (fs::directory_iterator it(folder, list_error), end; !list_error && it != end; it.increment(list_error)) {
        const std::string file = lower(ps2::os::utf8(it->path().filename()));
        if (file == l + ".fresh.ps2" || file == l + ".crash.ps2" || file == l + ".ps2.tmp" || file == l + ".ps2.lock" ||
            (file.rfind(l + ".damaged-", 0) == 0 && ends_with(file, ".ps2")))
            beside.push_back(it->path());
    }
    for (const fs::path &p : beside) fs::remove(p, error);
    return true;
}

bool rename(const std::string &dir, const std::string &from, const std::string &to, std::string &why) {
    if (to.empty()) {
        why = "no name";
        return false;
    }
    if (lower(from) != lower(to) && exists(dir, to)) {
        why = "a card called \"" + to + "\" is there already";
        return false;
    }
    if (!ps2::os::move_replacing(file_of(dir, from), file_of(dir, to), &why)) return false;
    std::error_code error;
    const fs::path old_backups = backups_of(dir, from), new_backups = backups_of(dir, to);
    if (fs::exists(old_backups, error)) {
        fs::rename(old_backups, new_backups, error);
        std::vector<fs::path> inside;
        for (fs::directory_iterator it(new_backups, error), end; !error && it != end; it.increment(error)) inside.push_back(it->path());
        const std::string old_prefix = from + "_";
        for (const fs::path &p : inside) {  // the copies are "<name>_<date>.ps2": they take the new name too
            const std::string file = ps2::os::utf8(p.filename());
            if (file.rfind(old_prefix, 0) != 0) continue;
            std::error_code e;
            fs::rename(p, new_backups / ps2::os::path(to + "_" + file.substr(old_prefix.size())), e);
        }
    }
    return true;
}

std::string when(const std::string &dir, const std::string &name) {
    std::error_code error;
    const fs::path p = ps2::os::path(file_of(dir, name));
    if (fs::file_size(p, error) == 0 || error) return {};
    const auto written = fs::last_write_time(p, error);
    if (error) return {};
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%d %b, %H:%M", &local);
    return text[0] == '0' ? text + 1 : text;
}

Start prepare(const std::string &dir, const std::string &seed_file, const std::string &want1, const std::string &want2) {
    Start s;
    std::error_code error;
    fs::create_directories(ps2::os::path(dir), error);
    std::vector<std::string> cards = list(dir);
    std::string first1, first2;
    if (cards.empty()) {
        std::string why;
        const std::string a = random_name(cards);
        const bool seeded = !seed_file.empty() && fs::file_size(ps2::os::path(seed_file), error) > 0 && !error;
        if (seeded) {
            fs::copy_file(ps2::os::path(seed_file), ps2::os::path(file_of(dir, a)), fs::copy_options::none, error);
            if (error) s.note = "the card " + seed_file + " could not be copied into the card library (" + error.message() + ")";
            else s.note = "the card " + seed_file + " was copied into the card library as \"" + a + "\" (the file itself stays as it is)";
        } else if (!create(dir, a, why)) {
            s.note = "no card could be made in " + dir + " (" + why + ")";
        }
        cards = list(dir);
        if (exists(dir, a)) first1 = a;
        const std::string b = random_name(cards);
        if (create(dir, b, why)) first2 = b;
        cards = list(dir);
        if (s.note.empty()) s.note = "new cards made in the card library";
    }
    const std::string want[2] = {want1, want2}, first[2] = {first1, first2};
    for (int slot = 0; slot < 2; ++slot) {
        if (want[slot] == "none") continue;
        const std::string name = want[slot].empty() ? first[slot] : want[slot];
        if (name.empty()) continue;
        for (const std::string &c : cards)
            if (lower(c) == lower(name)) s.slot[slot] = c;
    }
    if (!s.slot[0].empty() && lower(s.slot[0]) == lower(s.slot[1])) s.slot[1].clear();  // one card cannot be in both slots
    if (want1.empty() && want2.empty() && first1.empty() && first2.empty() && !cards.empty()) {
        // remaster.ini says nothing, but the folder has cards (the ini was deleted): the first one goes into slot 1
        s.slot[0] = cards[0];
    }
    return s;
}

}  // namespace cardlib
