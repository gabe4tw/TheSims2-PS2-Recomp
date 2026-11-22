// Local change: mods part 1 (host/mod_library.cpp), no game needed. Build: ninja ps2modlibtest, run
// build/.../ps2modlibtest [work folder] (prints "mod library: N of N checks passed, 0 failed").
#include "../host/mod_library.hpp"
#include "ps2/os.hpp"

#include <cstdio>
#include <cstdlib>
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
std::string names(const mods::Library &lib) {
    std::string out;
    for (const auto &m : lib.mods) out += (out.empty() ? "" : ",") + std::string(m.enabled ? "+" : "-") + m.folder;
    return out;
}
std::string u(const fs::path &p) { return ps2::os::utf8(p); }
bool has(const std::vector<std::string> &lines, const std::string &part) {
    for (const auto &l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
// a small mods folder: Alpha (settings + patches), Beta (textures), Gamma (another game)
fs::path three() {
    const fs::path d = fresh();
    write(d / "Alpha" / "mod.ini", "name = Alpha Mod\nauthor = Someone\nversion = 2\ngame = SLES-53718\ndescription = Faster\nurl = https://example.org\n");
    write(d / "Alpha" / "settings.ini", "# a comment\nPS2_REFRESH_HZ=60\nPS2_DEBAND=1\nNOT_OURS=1\n");
    write(d / "Alpha" / "patches.txt", "0 3db17c 4 3fe38e2a\n0 100000 4 1\n");
    write(d / "Alpha" / "notes.doc", "x");
    write(d / "Alpha" / "README.txt", "x");
    write(d / "Beta" / "mod.ini", "name = Beta Pack\ngame = SLES-53718\n");
    write(d / "Beta" / "textures" / "sub" / "a1-00000440.dds", "x");
    write(d / "Beta" / "textures" / "sub" / "b2-00000440.dds", "x");
    write(d / "Gamma" / "mod.ini", "name = Gamma\ngame = SLUS-21265\n");
    write(d / "Gamma" / "settings.ini", "PS2_DEBAND=0\n");
    fs::create_directories(d / ".unpack-old");
    return d;
}
}  // namespace

int main(int argc, char **argv) {
    g_work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ps2modlibtest";
    std::error_code ec;
    fs::remove_all(g_work, ec);
    fs::create_directories(g_work);

    {  // missing folder: empty
        mods::Library lib;
        check(mods::load(u(g_work / "nothere"), lib) && lib.mods.empty(), "missing folder is an empty library");
    }
    {  // read mods, defaults
        const fs::path d = three();
        mods::Library lib;
        check(mods::load(u(d), lib), "load");
        check(names(lib) == "+Alpha,+Beta,-Gamma", "new mods on, in name order; another game's off: " + names(lib));
        const mods::Mod &a = lib.mods[0];
        check(a.name == "Alpha Mod" && a.author == "Someone" && a.version == "2" && a.description == "Faster" && a.url == "https://example.org", "mod.ini fields");
        check(a.settings.size() == 2 && a.settings[0].first == "PS2_REFRESH_HZ" && a.settings[0].second == "60", "settings.ini: PS2_ lines only");
        check(a.patch_addresses.size() == 2 && a.patch_addresses[0] == 0x3db17c, "patches.txt addresses");
        check(a.ignored.size() == 1 && a.ignored[0] == "notes.doc", "ignored files listed (readme not)");
        check(lib.mods[1].textures && !lib.mods[0].textures, "textures folder seen");
        check(lib.mods[2].wrong_game() && !lib.mods[2].can_enable(), "another game's mod cannot be on");
        std::string why;
        check(!mods::set_enabled(lib, 2, true, &why) && why.find("SLUS-21265") != std::string::npos, "turning on another game's mod is refused");
        check(!fs::exists(d / "mods.ini"), "load writes nothing");
    }
    {  // mods.ini order, new on top, gone dropped, save
        const fs::path d = three();
        write(d / "mods.ini", "# order\noff Beta\non Alpha\non Gone\n");
        mods::Library lib;
        mods::load(u(d), lib);
        check(names(lib) == "-Gamma,-Beta,+Alpha", "listed in order, a new one on top: " + names(lib));
        check(mods::set_enabled(lib, 1, true), "turn on");
        check(read(d / "mods.ini").find("on Beta\non Alpha\n") != std::string::npos && read(d / "mods.ini").find("Gone") == std::string::npos, "saved; gone dropped");
        check(mods::move(lib, 2, -1) && names(lib) == "-Gamma,+Alpha,+Beta", "move up");
        check(!mods::move(lib, 0, -1), "the top cannot move up");
        mods::Library again;
        mods::load(u(d), again);
        check(names(again) == "-Gamma,+Alpha,+Beta", "the order is kept: " + names(again));
    }
    {  // plan: settings of the highest, patches lowest first, packs highest first
        const fs::path d = three();
        write(d / "Delta" / "mod.ini", "name = Delta\n");
        write(d / "Delta" / "settings.ini", "PS2_DEBAND=2\n");
        write(d / "Delta" / "patches.txt", "0 100000 4 2\n");
        write(d / "Delta" / "textures" / "c3-00000440.dds", "x");
        write(d / "mods.ini", "on Delta\non Alpha\non Beta\noff Gamma\n");
        mods::Library lib;
        mods::load(u(d), lib);
        const mods::Plan p = mods::plan(lib);
        check(p.names.size() == 3 && p.names[0] == "Delta", "plan: enabled names");
        bool deband = false;
        for (const auto &[n, v] : p.settings) deband = deband || (n == "PS2_DEBAND" && v == "2");
        check(deband && p.setting_owner.at("PS2_DEBAND") == "Delta" && p.setting_owner.at("PS2_REFRESH_HZ") == "Alpha Mod", "plan: the highest mod's setting");
        check(p.patch_files.size() == 2 && p.patch_files[0].find("Alpha") != std::string::npos && p.patch_files[1].find("Delta") != std::string::npos,
              "plan: patches lowest first (the higher one applied last)");
        check(p.texture_packs.size() == 2 && p.texture_packs[0].first.find("Delta") != std::string::npos && p.texture_packs[1].second.find(".cache") != std::string::npos,
              "plan: packs highest first, cache folders");
        check(mods::packs_text(p.texture_packs).find('|') != std::string::npos && mods::packs_text(p.texture_packs).find('*') != std::string::npos, "packs text");
        const auto owners = mods::owners_from(mods::owners_text(p.setting_owner));
        check(owners == p.setting_owner, "owners text round trip");
        check(mods::owners_from("PS2_A=x;bad;PS2_B=y").size() == 2, "owners text: bad items skipped");
        // conflicts
        write(d / "Beta" / "textures" / "c3-00000440.dds", "x");
        write(d / "Beta" / ".cache" / "matches.txt", "00000000000000aa sub/a1-00000440.dds\n00000000000000bb -\n");
        write(d / "Delta" / ".cache" / "matches.txt", "00000000000000aa c3-00000440.dds\n00000000000000bb c3-00000440.dds\n");
        mods::load(u(d), lib);
        const auto alpha = mods::conflicts(lib, 1);
        check(has(alpha, "Patches: 1 address also patched by \"Delta\" (\"Delta\" wins") && has(alpha, "Setting PS2_DEBAND also set by \"Delta\""), "conflicts: patches, settings");
        const auto beta = mods::conflicts(lib, 2);
        check(has(beta, "Textures: 1 picture also in \"Delta\"") && has(beta, "Textures: 1 game texture also replaced by \"Delta\""), "conflicts: pictures, matched textures");
        check(!has(mods::conflicts(lib, 0), "Gamma"), "conflicts: mods that are off do not count");
    }
    {  // remove
        const fs::path d = three();
        write(d / "Alpha.zip", "zip");
        mods::Library lib;
        mods::load(u(d), lib);
        check(mods::remove(lib, 0) && !fs::exists(d / "Alpha") && !fs::exists(d / "Alpha.zip") && names(lib) == "+Beta,-Gamma", "delete: folder and zip gone");
    }
    {  // zips: the new ones, install (top folder inside), not a mod
        const fs::path d = fresh();
        write(d / "One.zip", "x");
        write(d / "Two.zip", "x");
        fs::create_directories(d / "Two");
        const auto z = mods::new_zips(u(d));
        check(z.size() == 1 && z[0].find("One.zip") != std::string::npos, "new zips: only those without a folder");
        const mods::Extract fake_nested = [](const std::string &, const std::string &into, std::string &) {
            write(fs::path(ps2::os::path(into)) / "One-1.0" / "mod.ini", "name = One\n");
            write(fs::path(ps2::os::path(into)) / "One-1.0" / "settings.ini", "PS2_DEBAND=1\n");
            return true;
        };
        std::string folder, why;
        check(mods::install_zip(u(d), u(d / "One.zip"), fake_nested, folder, why) && folder == "One" && fs::exists(d / "One" / "mod.ini"), "install: top folder's contents " + why);
        check(!fs::exists(d / ".unpack-One"), "install: no temp folder left");
        check(!mods::install_zip(u(d), u(d / "One.zip"), fake_nested, folder, why), "install: refused when the folder exists");
        const mods::Extract junk = [](const std::string &, const std::string &into, std::string &) {
            write(fs::path(ps2::os::path(into)) / "photo.jpg", "x");
            return true;
        };
        write(d / "Three.zip", "x");
        check(!mods::install_zip(u(d), u(d / "Three.zip"), junk, folder, why) && why.find("not a mod") != std::string::npos && !fs::exists(d / "Three"), "install: not a mod");
#ifdef _WIN32
        // a real zip made and unpacked by Windows' tar.exe
        const fs::path src = fresh();
        write(src / "Real Mod" / "mod.ini", "name = Real\n");
        write(src / "Real Mod" / "patches.txt", "0 100000 4 1\n");
        const std::string zip = u(d / "Real.zip");
        const std::string make = "C:\\Windows\\System32\\tar.exe -a -cf \"" + zip + "\" -C \"" + u(src) + "\" \"Real Mod\"";
        if (std::system(make.c_str()) == 0) {
            const mods::Extract tar = [](const std::string &file, const std::string &into, std::string &why) {
                const std::string cmd = "C:\\Windows\\System32\\tar.exe -xf \"" + file + "\" -C \"" + into + "\"";
                if (std::system(cmd.c_str()) == 0) return true;
                why = "tar failed";
                return false;
            };
            check(mods::install_zip(u(d), zip, tar, folder, why) && fs::exists(d / "Real" / "patches.txt"), "install: a real zip " + why);
        } else check(false, "tar.exe could not make a zip");
#endif
    }
    std::printf("mod library: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    return failed ? 1 : 0;
}
