// Local change: the launcher's install logic (host/launcher_install.cpp), no game build needed. The disc cases make small disc images
// with scripts/mkiso.py (Python 3 on PATH; without it they are left out with a note). Build: ninja ps2installtest, run from the repo
// root: build/.../ps2installtest [work folder].
#include "../host/launcher_config.hpp"
#include "../host/hd_pack.hpp"
#include "../host/launcher_install.hpp"
#include "../host/launcher_os.hpp"
#include "ps2/os.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <thread>
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
fs::path g_work;
// a disc image whose SYSTEM.CNF boots `boot` (8.3 name); false when Python or mkiso.py is not there
bool make_iso(const fs::path &iso, const std::string &boot) {
    const fs::path dir = iso.parent_path();
    write(dir / boot, "x");
    const std::string cmd = "cd /d \"" + dir.string() + "\" && python -I \"" + fs::absolute("scripts/mkiso.py").string() + "\" \"" + iso.filename().string() + "\" " + boot + " > nul 2>&1";
    return std::system(cmd.c_str()) == 0 && fs::exists(iso);
}
}  // namespace

int main(int argc, char **argv) {
    g_work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ps2installtest";
    std::error_code error;
    fs::remove_all(g_work, error);
    fs::create_directories(g_work);
    std::string boot, why;
    {
        const fs::path d = g_work / "discs";
        fs::create_directories(d);
        if (make_iso(d / "sles.iso", "SLES_537.18") && make_iso(d / "slus.iso", "SLUS_212.65") && make_iso(d / "other.iso", "SLES_123.45")) {
            check(check_disc(ps2::os::utf8(d / "sles.iso"), boot, why) == DiscKind::sles53718 && boot == "SLES_537.18", "disc_sles");
            check(check_disc(ps2::os::utf8(d / "slus.iso"), boot, why) == DiscKind::slus21265, "disc_slus");
            check(check_disc(ps2::os::utf8(d / "other.iso"), boot, why) == DiscKind::other && boot == "SLES_123.45", "disc_other");
        } else {
            std::fprintf(stderr, "note: no Python / mkiso.py: the made-up disc cases were left out\n");
        }
        check(check_disc(ps2::os::utf8(d / "missing.iso"), boot, why) == DiscKind::unreadable && !why.empty(), "disc_missing_file");
        const char *real = "D:\\PCSX2\\_games\\Sims 2, The (Europe) (En,Fr,De,Es,It,Nl,Sv,No,Da,Fi).iso";
        if (fs::exists(real)) check(check_disc(real, boot, why) == DiscKind::sles53718, "disc_real_sles");
        check(disc_message(DiscKind::slus21265) == "The US version (SLUS-21265) is not supported yet." &&
                  disc_message(DiscKind::other) == "This is not The Sims 2 (PAL, SLES-53718)." && disc_message(DiscKind::sles53718).empty(),
              "disc_messages");
    }
    {
        const fs::path d = g_work / "find";
        write(d / "b.iso", "");
        write(d / "a.ISO", "");
        write(d / "c.txt", "");
        const auto found = find_isos(ps2::os::utf8(d));
        check(found.size() == 2 && fs::path(found[0]).filename() == "a.ISO" && fs::path(found[1]).filename() == "b.iso", "find_isos_sorted");
    }
    {  // sha256
        const fs::path d = g_work / "hash";
        write(d / "empty", "");
        write(d / "abc", "abc");
        check(sha256_file(ps2::os::utf8(d / "empty")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
                  sha256_file(ps2::os::utf8(d / "abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "sha256_known");
        check(sha256_file(ps2::os::utf8(d / "missing")).empty(), "sha256_missing_file");
    }
    {  // downloads from a local web server
        const fs::path served = g_work / "served", into = g_work / "got";
        fs::create_directories(into);
        write(served / "tool.zip", "abc");
        const int port = 18765;
        const std::string server = "start \"\" /b python -I \"" + fs::absolute("tests/launcher_net_server.py").string() + "\" \"" + served.string() + "\" " +
                                   std::to_string(port) + " > nul 2>&1";
        if (std::system(server.c_str()) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/";
            Download good{"tool", base + "tool.zip", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 3, Download::Unpack::none, "tool"};
            check(fetch_checked(good, ps2::os::utf8(into), nullptr, why) && fs::file_size(into / "tool.zip") == 3, "fetch_served");
            const std::size_t before = std::count(std::istreambuf_iterator<char>(std::ifstream(served / "requests.log").rdbuf()), {}, '\n');
            check(fetch_checked(good, ps2::os::utf8(into), nullptr, why), "fetch_reuses_good_file: ok");
            const std::size_t after = std::count(std::istreambuf_iterator<char>(std::ifstream(served / "requests.log").rdbuf()), {}, '\n');
            check(before == after, "fetch_reuses_good_file: no request made");
            Download bad = good;
            bad.sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
            bad.url = base + "tool.zip?x";
            fs::remove(into / "tool.zip");
            check(!fetch_checked(bad, ps2::os::utf8(into), nullptr, why) && !fs::exists(into / "tool.zip") && !fs::exists(into / "tool.zip.part"),
                  "fetch_bad_hash_discarded");
            check(!fetch_checked(good, ps2::os::utf8(into), [](std::uint64_t, std::uint64_t) { return false; }, why) && !fs::exists(into / "tool.zip.part"),
                  "fetch_cancel");
            Download missing = good;
            missing.url = base + "nothing.zip";
            check(!fetch_checked(missing, ps2::os::utf8(into), nullptr, why) && why.find("404") != std::string::npos, "fetch_not_found_says_so");
            std::system("for /f \"tokens=5\" %p in ('netstat -ano ^| findstr :18765 ^| findstr LISTENING') do taskkill /PID %p /F > nul 2>&1");
        } else {
            std::fprintf(stderr, "note: no Python: the download cases were left out\n");
        }
    }
    {  // unpacking with Windows' own tar.exe
        const fs::path d = g_work / "unpack";
        write(d / "in" / "bin" / "x.txt", "hello");
        const std::string zip = ps2::os::utf8(d / "a.zip"), sevenzip = ps2::os::utf8(d / "a.7z");
        std::system(("cd /d \"" + (d / "in").string() + "\" && C:\\Windows\\System32\\tar.exe -a -cf \"" + zip + "\" bin > nul 2>&1").c_str());
        Download z{"z", "", "", 0, Download::Unpack::zip, "z"};
        check(unpack(z, zip, ps2::os::utf8(d / "out_zip"), why) && fs::exists(d / "out_zip" / "bin" / "x.txt"), "unpack_zip");
        std::system(("cd /d \"" + (d / "in").string() + "\" && C:\\Windows\\System32\\tar.exe --format 7zip -cf \"" + sevenzip + "\" bin > nul 2>&1").c_str());
        if (fs::exists(sevenzip)) {
            Download s{"s", "", "", 0, Download::Unpack::sevenzip, "s"};
            check(unpack(s, sevenzip, ps2::os::utf8(d / "out_7z"), why) && fs::exists(d / "out_7z" / "bin" / "x.txt"), "unpack_7z");
        } else {
            std::fprintf(stderr, "note: this tar.exe cannot write 7z: unpack_7z left out (reading is checked by the HD pack in stage 3)\n");
        }
    }
    {  // the build's PATH: only the downloaded tools and System32
        const std::string path = tool_path("C:\\T");
        bool only = true;
        std::size_t start = 0;
        while (start <= path.size()) {
            const std::size_t end = path.find(';', start);
            const std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty() && part.rfind("C:\\T\\", 0) != 0 && part != "C:\\Windows\\System32") only = false;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        check(only && path.find("git\\usr\\bin") != std::string::npos && path.find("cmake") != std::string::npos && path.find("python") != std::string::npos,
              "clean_path_only_tools");
        check(tool_downloads().size() == 5, "tool_downloads: llvm-mingw, cmake, ninja, PortableGit, Python");
        for (const Download &t : tool_downloads()) check(t.sha256.size() == 64 && t.size > 0 && t.url.rfind("https://", 0) == 0, "tool_downloads pinned: " + t.name);
    }
    // ---- Task 4: the install steps with a stand-in source (its install script only prints the step lines and makes a fake game) ----
    std::string bash = ps2::os::env("PS2_TEST_BASH");
    if (bash.empty()) bash = "C:\\PS2Native\\scratch_install\\tools\\git\\usr\\bin\\bash.exe";
    if (!fs::exists(bash)) {
        std::fprintf(stderr, "note: no bash (PS2_TEST_BASH): the install step cases were left out\n");
    } else {
        // source zips: <name>.zip holding "Repo-<v>/scripts/sims2_install.sh" (as GitHub's tag zips have one top folder)
        const auto make_source = [&](const std::string &version, bool build_ok) {
            const fs::path d = g_work / "zips" / version, top = d / ("Repo-" + version);
            std::string script = "#!/usr/bin/env bash\nfor n in 1 2 3 4 5; do echo \"[install] step $n/5: stand-in\"; done\n";
            script += build_ok ? "mkdir -p build/native-sims2 && echo game > build/native-sims2/ps2run.exe\necho data > games/sims2/written-by-" + version + "\necho \"[install] done\"\n"
                               : "echo \"[install] failed: stand-in build failure\" >&2\nexit 1\n";
            write(top / "scripts" / "sims2_install.sh", script);
            const fs::path zip = g_work / "zips" / (version + ".zip");
            std::system(("cd /d \"" + d.string() + "\" && C:\\Windows\\System32\\tar.exe -a -cf \"" + zip.string() + "\" Repo-" + version + " > nul 2>&1").c_str());
            return ps2::os::utf8(zip);
        };
        const fs::path base = (g_work / ps2::os::path("player folder ë")).lexically_normal();  // (UTF-8 name: through ps2::os::path)
        const Layout l = layout_for(ps2::os::utf8(base));
        check(ps2::os::path(l.tools).lexically_normal() == base / "tools" && ps2::os::path(l.data).lexically_normal() == base / "data" / "sims2" && ps2::os::path(l.source("v1")).lexically_normal() == base / "source" / "v1", "layout_paths");
        InstallState s;
        s.iso = "D:\\x.iso";
        s.version = "v1";
        s.reached = Step::tools;
        check(save_state(l, s), "state_round_trip: saved");
        InstallState back;
        check(load_state(l, back) && back.iso == s.iso && back.version == "v1" && back.reached == Step::tools, "state_round_trip");

        InstallSetup setup;
        setup.tools = {};  // (no tools in the test)
        setup.bash = bash;
        setup.path = ps2::os::utf8(fs::path(bash).parent_path()) + ";C:\\Windows\\System32";
        setup.source_url = make_source("v1", true);
        setup.free_space = [](const std::string &) { return std::uint64_t(1) << 30; };  // 1 GB
        std::atomic<bool> cancel{false};
        std::vector<std::string> lines;
        const auto report = [&](const std::string &line, double) { lines.push_back(line); };
        s = InstallState{};
        s.iso = "D:\\x.iso";
        s.version = "v1";
        check(!run_install(l, s, setup, report, cancel) && s.error.find("3 GB") != std::string::npos && s.reached == Step::space, "space_check");
        setup.free_space = [](const std::string &) { return std::uint64_t(100) << 30; };
        lines.clear();
        const bool installed = run_install(l, s, setup, report, cancel);
        check(installed && s.reached == Step::done, "install_runs_all_steps: " + s.error);
        int step_lines = 0;
        for (const std::string &x : lines) step_lines += x.rfind("[install] step", 0) == 0;
        check(step_lines == 5, "install_reports_the_step_lines");
        check(fs::exists(base / "source" / "v1" / "build" / "native-sims2" / "ps2run.exe"), "install_builds_into_source_v1");
        check(fs::exists(base / "data" / "sims2" / "written-by-v1"), "junction_shares_data: written through source\\v1\\games\\sims2");
        std::string why2;
        check(write_installed_config(l, s, why2), "installed_config: written");
        LaunchConfig c;
        check(read_launch_config(ps2::os::utf8(base / "launcher.ini"), c, why2) && c.iso == "D:\\x.iso" &&
                  ps2::os::path(under_root(c, c.runner)).lexically_normal() == base / "source" / "v1" / "build" / "native-sims2" / "ps2run.exe",
              "installed_config: read back, paths under source\\v1");
        check(installed_version(l) == "v1", "installed_version");

        // resume: a state that already reached the build step does not download the source again
        s.reached = Step::build;
        setup.source_url = ps2::os::utf8(g_work / "zips" / "nothing-there.zip");
        check(run_install(l, s, setup, report, cancel) && s.reached == Step::done, "resume_after_interrupt: " + s.error);

        // updates
        InstallState u = s;
        setup.source_url = make_source("v2", false);
        check(!run_update(l, u, "v2", setup, report, cancel) && installed_version(l) == "v1" && fs::exists(base / "source" / "v1" / "build" / "native-sims2" / "ps2run.exe"),
              "failed_update_keeps_old");
        // local change (stage 3): the HD textures come with the game, into the data folder; one that cannot be had fails the step
        const std::string hd_archive = "C:\\PS2Native\\SLUS-21265.7z";
        const bool hd_test = fs::exists(ps2::os::path(hd_archive));
        if (hd_test) {
            setup.hd_textures = true;
            setup.hd_local = ps2::os::utf8(g_work / "zips" / "no-such-archive.7z");
            setup.source_url = make_source("v3", true);
            u = s;
            check(!run_update(l, u, "v3", setup, report, cancel) && installed_version(l) == "v1" && !hdpack::installed(l.data), "hd_failure_fails_the_update");
            setup.hd_local = hd_archive;
        }
        setup.source_url = make_source("v3", true);
        u = s;
        check(run_update(l, u, "v3", setup, report, cancel) && installed_version(l) == "v3" && fs::exists(base / "source" / "v1"), "update_switches_after_build: " + u.error);
        if (hd_test) {
            std::size_t pictures = 0;
            std::error_code none;
            for (const auto &e : fs::directory_iterator(ps2::os::path(hdpack::pictures_dir(l.data)), none)) pictures += e.path().extension() == ".dds";
            check(hdpack::installed(l.data) && pictures == 2592 && fs::exists(base / "source" / "v3" / "games" / "sims2" / "textures" / "hd"),
                  "hd_textures_with_the_update: " + std::to_string(pictures) + " pictures, seen through the junction");
        } else std::printf("(hd textures checks skipped: no C:\\PS2Native\\SLUS-21265.7z)\n");
        check(fs::exists(base / "data" / "sims2" / "written-by-v1") && fs::exists(base / "data" / "sims2" / "written-by-v3"), "update_shares_the_data");
        remove_other_versions(l, "v3");
        check(!fs::exists(base / "source" / "v1") && !fs::exists(base / "source" / "v2") && fs::exists(base / "source" / "v3") &&
                  fs::exists(base / "data" / "sims2" / "written-by-v1"),
              "old_versions_removed_but_data_kept");
    }
    if (fs::exists(bash)) {
        // build_without_stdin: a launcher started by a double-click has no stdin; the build's tools (sha256sum) must still work
        const fs::path d = g_work / "nostdin";
        write(d / "f.txt", "abc");
        const HANDLE kept = GetStdHandle(STD_INPUT_HANDLE);
        SetStdHandle(STD_INPUT_HANDLE, nullptr);
        std::atomic<bool> never{false};
        int code = -1;
        std::string out;
        const bool ran = run_with_output(bash, {"-c", "sha256sum f.txt"}, ps2::os::utf8(d), ps2::os::utf8(fs::path(bash).parent_path()) + ";C:\\Windows\\System32",
                                         [&](const std::string &line) { out += line + "\n"; }, never, code, why);
        SetStdHandle(STD_INPUT_HANDLE, kept);
        check(ran && code == 0 && out.find("ba7816bf8f01cfea") != std::string::npos, "build_without_stdin: " + out);
    }
    std::printf("install: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    if (!failed) fs::remove_all(g_work, error);
    return failed ? 1 : 0;
}
