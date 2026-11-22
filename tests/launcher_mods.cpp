// Local change: the launcher's Mods page helpers (host/launcher_mods.cpp), no window and no network. Build: ninja ps2launchermodstest
//   ps2launchermodstest [work folder] [--real <SLUS-21265.7z>]   (--real: the HD pack's own archive through fetch_hd's local copy + install_hd)
// prints "launcher mods: N of N checks passed, 0 failed".
#include "../host/hd_pack.hpp"
#include "../host/launcher_mods.hpp"
#include "../host/remaster_ini.hpp"
#include "ps2/os.hpp"

#include <cstdio>
#include <cstdlib>
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
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}
std::string u(const fs::path &p) { return ps2::os::utf8(p); }
fs::path g_work;
int g_case = 0;
fs::path fresh() {
    const fs::path d = g_work / ("case" + std::to_string(++g_case));
    std::error_code e;
    fs::remove_all(d, e);
    fs::create_directories(d);
    return d;
}
bool tar(const std::string &args) { return std::system(("C:\\Windows\\System32\\tar.exe " + args).c_str()) == 0; }
}  // namespace

int main(int argc, char **argv) {
    g_work = fs::temp_directory_path() / "ps2launchermodstest";
    std::string real;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--real" && i + 1 < argc) real = argv[++i];
        else g_work = fs::path(a);
    }
    std::error_code ec;
    fs::remove_all(g_work, ec);
    fs::create_directories(g_work);

    {  // Google Drive's "can't scan for viruses" page -> the real link
        const std::string page =
            "<!DOCTYPE html><html><head><title>Google Drive - Virus scan warning</title></head><body><p>Google Drive can't scan this file for viruses.</p>"
            "<form id=\"download-form\" action=\"https://drive.usercontent.google.com/download\" method=\"get\">"
            "<input type=\"submit\" id=\"uc-download-link\" class=\"goog-inline-block jfk-button jfk-button-action\" value=\"Download anyway\"/>"
            "<input type=\"hidden\" name=\"id\" value=\"1nTJ07wGYAB1MyYHGEXiSBsCeVTeN2QGY\"><input type=\"hidden\" name=\"export\" value=\"download\">"
            "<input type=\"hidden\" name=\"confirm\" value=\"t\"><input type=\"hidden\" name=\"uuid\" value=\"a1b2-c3d4\"></form></body></html>";
        check(hdpack::drive_confirm_url(page) ==
                  "https://drive.usercontent.google.com/download?id=1nTJ07wGYAB1MyYHGEXiSBsCeVTeN2QGY&export=download&confirm=t&uuid=a1b2-c3d4",
              "drive page: the form's link");
        check(hdpack::drive_confirm_url("<html><body>Too many users have viewed or downloaded this file recently.</body></html>").empty(), "drive page: no form");
        check(hdpack::drive_confirm_url("<form action=\"/relative\"><input name=\"a\" value=\"b\"></form>").empty(), "drive page: only https links");
    }
    {  // rows and settings
        const fs::path d = fresh();
        write(d / "Look" / "mod.ini", "name = Look\nauthor = Ann\nversion = 3\ndescription = Smoother bands.\n");
        write(d / "Look" / "settings.ini", "PS2_DEBAND=2\nPS2_UI_SQUEEZE=1\n");
        write(d / "Look" / "textures" / "a1-00000440.dds", "x");
        write(d / "Look" / "extra.bin", "x");
        write(d / "Base" / "settings.ini", "PS2_DEBAND=0\n");
        write(d / "US only" / "mod.ini", "game = SLUS-21265\n");
        write(d / "mods.ini", "on Look\non Base\n");
        mods::Library lib;
        mods::load(u(d), lib);
        const auto v = launchermods::rows(lib);
        check(v.rows.size() == 3, "rows: three mods");
        const auto &look = v.rows[1];
        check(look.name == "Look" && look.info == "by Ann, version 3" && look.on, "rows: name and info");
        check(look.details.find("Smoother bands.") == 0 && look.details.find("textures (1 pictures)") != std::string::npos &&
                  look.details.find("PS2_DEBAND=2") != std::string::npos && look.details.find("Ignored: extra.bin") != std::string::npos,
              "rows: details " + look.details);
        check(look.warnings.size() == 1 && look.warnings[0].find("PS2_DEBAND also set by \"Base\"") != std::string::npos, "rows: the conflict is a warning");
        check(v.rows[0].name == "US only" && !v.rows[0].can_on && !v.rows[0].warnings.empty(), "rows: another game's mod");
        check(v.rows[2].details.find("(No mod.ini") != std::string::npos, "rows: no mod.ini noted");
        remaster::g_options[5].now = remaster::g_options[5].next = 0;
        launchermods::apply_settings(lib);
        check(remaster::g_options[5].next == 2 && remaster::g_options[1].next == 75 && remaster::mod_owner(5) == "Look", "settings: the highest mod's values, owned");
        remaster::g_mod_owned.clear();
    }
    {  // zips dropped into the mods folder, made by tar.exe
        const fs::path d = fresh(), src = fresh();
        write(src / "Zipped" / "mod.ini", "name = Zipped\n");
        write(src / "Zipped" / "patches.txt", "0 100000 4 1\n");
        if (tar("-a -cf \"" + u(d / "Zipped.zip") + "\" -C \"" + u(src) + "\" Zipped")) {
            const auto lines = launchermods::unpack_new_zips(u(d));
            check(lines.size() == 1 && lines[0].find("Unpacked Zipped.zip") == 0 && fs::exists(d / "Zipped" / "patches.txt"), "zip in the folder: unpacked");
            check(launchermods::unpack_new_zips(u(d)).empty(), "zip in the folder: only once");
        } else check(false, "tar.exe could not make a zip");
    }
    {  // the HD textures (part of the game, host/hd_pack.hpp): a made-up 7z of the official layout, a wrong checksum
        const fs::path game = fresh(), src = fresh(), downloads = fresh();
        write(src / "SLUS-21265" / "replacements" / "dds" / "a1-00000440.dds", "x");
        write(src / "SLUS-21265" / "replacements" / "dds" / "b2-00000440.dds", "x");
        const std::string archive = u(downloads / "small.7z");
        check(!hdpack::installed(u(game)) && hdpack::size_text() == "98 MB", "hd: not there yet");
        if (tar("--format 7zip -cf \"" + archive + "\" -C \"" + u(src) + "\" SLUS-21265")) {
            std::string why;
            check(hdpack::install(u(game), archive, why), "hd install: " + why);
            check(fs::exists(game / "textures" / "hd" / "b2-00000440.dds") && !fs::exists(archive) && !fs::exists(game / "textures" / ".unpack-hd") && hdpack::installed(u(game)),
                  "hd install: pictures in textures\\hd, archive and temp gone");
            write(downloads / "fake.7z", "not the pack");
            std::string file;
            check(!hdpack::fetch(u(downloads / "dl"), u(downloads / "fake.7z"), {}, file, why) && why.find("checksum") != std::string::npos &&
                      !fs::exists(downloads / "dl" / "SLUS-21265.7z.part") && !fs::exists(downloads / "dl" / "SLUS-21265.7z"),
                  "hd fetch: a wrong file is thrown away");
        } else check(false, "tar.exe could not make a 7z");
    }
    if (!real.empty()) {  // the real archive: copied, checked, unpacked into a game folder
        const fs::path game = fresh(), downloads = fresh();
        std::string file, why;
        const bool fetched = hdpack::fetch(u(downloads), real, {}, file, why);
        check(fetched, "real: fetch " + why);
        std::string again;
        check(fetched && hdpack::fetch(u(downloads), "Z:/no/such/file.7z", {}, again, why), "real: a good file there already is kept (no copy, no request)");
        check(fetched && hdpack::install(u(game), file, why), "real: install " + why);
        std::size_t n = 0;
        for (const auto &e : fs::directory_iterator(game / "textures" / "hd", ec)) n += e.path().extension() == ".dds";
        check(n == 2592, "real: 2592 pictures in textures\\hd, found " + std::to_string(n));
        check(fs::exists(game / "textures" / ".hd-cache" / "thumbs.bin"), "real: the game's index is made at install");
    }
    std::printf("launcher mods: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    return failed ? 1 : 0;
}
