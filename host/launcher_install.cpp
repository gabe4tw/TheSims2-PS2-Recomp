#include "launcher_install.hpp"

#include "hd_pack.hpp"

#include "ps2/disc.hpp"
#include "ps2/os.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <filesystem>

namespace {
std::string upper(std::string t) {
    for (char &c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return t;
}
}  // namespace

DiscKind check_disc(const std::string &iso, std::string &boot_name, std::string &why) {
    boot_name.clear();
    std::error_code e;
    if (!std::filesystem::is_regular_file(ps2::os::path(iso), e)) {
        why = "The disc image " + iso + " is not there.";
        return DiscKind::unreadable;
    }
    if (!ps2::disc::open(iso)) {
        why = "The file " + iso + " cannot be read as a PS2 disc image.";
        return DiscKind::unreadable;
    }
    const auto cnf = ps2::disc::find("SYSTEM.CNF");
    if (!cnf || cnf->directory || cnf->size == 0 || cnf->size > 65536) {
        why = "The disc image has no SYSTEM.CNF: it is not a PS2 game disc.";
        return DiscKind::unreadable;
    }
    std::vector<char> text((cnf->size + ps2::disc::kSector - 1) / ps2::disc::kSector * ps2::disc::kSector);
    if (!ps2::disc::read(cnf->sector, static_cast<std::uint32_t>(text.size() / ps2::disc::kSector), text.data())) {
        why = "The disc image's SYSTEM.CNF cannot be read.";
        return DiscKind::unreadable;
    }
    const std::string all = upper(std::string(text.data(), cnf->size));
    const std::size_t at = all.find("BOOT2");
    if (at == std::string::npos) {
        why = "SYSTEM.CNF names no program (BOOT2): it is not a PS2 game disc.";
        return DiscKind::unreadable;
    }
    std::string line = all.substr(at, all.find_first_of("\r\n", at) - at);
    std::size_t start = line.find_last_of(":\\/");
    start = start == std::string::npos ? line.find('=') : start;
    line = line.substr(start == std::string::npos ? 0 : start + 1);
    line = line.substr(0, line.find(';'));
    line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
    boot_name = line;
    if (boot_name == "SLES_537.18") return DiscKind::sles53718;
    if (boot_name == "SLUS_212.65") return DiscKind::slus21265;
    return DiscKind::other;
}

std::string disc_message(DiscKind kind) {
    switch (kind) {
    case DiscKind::sles53718: return {};
    case DiscKind::slus21265: return "The US version (SLUS-21265) is not supported yet.";
    case DiscKind::other: return "This is not The Sims 2 (PAL, SLES-53718).";
    default: return "This file cannot be read as a PS2 disc image.";
    }
}

std::vector<std::string> find_isos(const std::string &dir) {
    std::vector<std::string> out;
    std::error_code error;
    for (std::filesystem::directory_iterator it(ps2::os::path(dir), error), end; !error && it != end; it.increment(error)) {
        std::error_code e;
        if (!it->is_regular_file(e)) continue;
        if (upper(ps2::os::utf8(it->path().extension())) == ".ISO") out.push_back(ps2::os::utf8(it->path()));
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return upper(a) < upper(b); });
    return out;
}

// ---- downloads ----

#include "launcher_net.hpp"
#include "launcher_os.hpp"

const std::vector<Download> &tool_downloads() {
    // pinned 2026-11-17 (the versions this PC builds with; PortableGit and Python: the current releases), sizes and sha256 measured
    static const std::vector<Download> table = {
        {"llvm-mingw 20261020 (C/C++ compiler)", "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip",
         "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd", 190612575, Download::Unpack::zip, "llvm-mingw"},
        {"CMake 4.5.0", "https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-windows-x86_64.zip",
         "301b1728317ccd84e984b02e366a8293735bc5ac618757d3d8f58b578f062411", 56341654, Download::Unpack::zip, "cmake"},
        {"Ninja 1.13.2", "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
         "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", 291570, Download::Unpack::zip, "ninja"},
        {"PortableGit 2.56.1 (bash, curl, unzip)", "https://github.com/git-for-windows/git/releases/download/v2.56.1.windows.1/PortableGit-2.56.1-64-bit.7z.exe",
         "9dd04a608ceb5f6ab32a73709e0ecb159d211da71b697c45ff81f6dd0c6b49c7", 60125512, Download::Unpack::portable_git, "git"},
        {"Python 3.14.7 (embeddable)", "https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip",
         "d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15", 12673227, Download::Unpack::zip, "python"},
    };
    return table;
}

std::string sha256_file(const std::string &file) { return sha256_of_file(file); }

std::string download_file_name(const Download &d) {
    std::string name = d.url.substr(d.url.find_last_of('/') + 1);
    name = name.substr(0, name.find('?'));
    return name.empty() ? d.folder + ".download" : name;
}

bool fetch_checked(const Download &d, const std::string &dir, const Progress &progress, std::string &why) {
    namespace fs = std::filesystem;
    std::error_code e;
    fs::create_directories(ps2::os::path(dir), e);
    const std::string file = ps2::os::utf8(ps2::os::path(dir) / ps2::os::path(download_file_name(d)));
    const std::string part = file + ".part";
    if (fs::is_regular_file(ps2::os::path(file), e) && sha256_file(file) == d.sha256) return true;  // (there already, and right)
    fs::remove(ps2::os::path(file), e);
    if (!http_get(d.url, part, progress, why)) {
        fs::remove(ps2::os::path(part), e);
        return false;
    }
    const std::string got = sha256_file(part);
    if (got != d.sha256) {
        fs::remove(ps2::os::path(part), e);
        why = d.name + ": the downloaded file is not the expected one (its checksum differs), so it was deleted";
        return false;
    }
    fs::rename(ps2::os::path(part), ps2::os::path(file), e);
    if (e) {
        why = "cannot rename " + part + " (" + e.message() + ")";
        return false;
    }
    return true;
}

bool unpack(const Download &d, const std::string &file, const std::string &into, std::string &why) {
    std::error_code e;
    std::filesystem::create_directories(ps2::os::path(into), e);
    int code = -1;
    bool started = false;
    const std::atomic<bool> never{false};
    const std::string system = "C:\\Windows\\System32";
    switch (d.unpack) {
    case Download::Unpack::none: return true;
    case Download::Unpack::zip:
    case Download::Unpack::sevenzip:
        started = run_with_output(system + "\\tar.exe", {"-xf", file, "-C", into}, into, system, nullptr, never, code, why);
        break;
    case Download::Unpack::portable_git: started = run_with_output(file, {"-o" + into, "-y"}, into, system, nullptr, never, code, why); break;
    }
    if (!started) return false;
    if (code != 0) {
        why = d.name + ": unpacking failed (code " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

std::string tool_path(const std::string &tools_dir) {
    const std::string t = tools_dir;
    return t + "\\git\\usr\\bin;" + t + "\\git\\ucrt64\\bin;" + t + "\\llvm-mingw\\llvm-mingw-20261020-ucrt-x86_64\\bin;" + t +
           "\\cmake\\cmake-4.5.0-windows-x86_64\\bin;" + t + "\\ninja;" + t + "\\python;C:\\Windows\\System32";
}

// ---- the install ----

namespace {
namespace fs = std::filesystem;
std::string join(const std::string &dir, const std::string &name) { return ps2::os::utf8(ps2::os::path(dir) / ps2::os::path(name)); }
bool exists_path(const std::string &p) {
    std::error_code e;
    return fs::exists(ps2::os::path(p), e);
}
}  // namespace

std::string Layout::source(const std::string &version) const { return join(source_root, version); }

Layout layout_for(const std::string &base) {
    Layout l;
    l.base = base;
    l.tools = join(base, "tools");
    l.downloads = join(base, "downloads");
    l.data = ps2::os::utf8(ps2::os::path(base) / "data" / "sims2");
    l.source_root = join(base, "source");
    return l;
}

bool load_state(const Layout &l, InstallState &s) {
    std::FILE *f = ps2::os::open(join(l.base, "install.ini"), "rb");
    if (!f) return false;
    InstallState got;
    char line[2048];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = t.substr(0, eq), v = t.substr(eq + 1);
        if (k == "iso") got.iso = v;
        else if (k == "version") got.version = v;
        else if (k == "reached") got.reached = static_cast<Step>(std::clamp(std::atoi(v.c_str()), 0, static_cast<int>(Step::done)));
        else if (k == "error") got.error = v;
    }
    std::fclose(f);
    s = got;
    return true;
}

bool save_state(const Layout &l, const InstallState &s) {
    std::error_code e;
    fs::create_directories(ps2::os::path(l.base), e);
    std::string error = s.error;
    std::replace(error.begin(), error.end(), '\n', ' ');
    const std::string text = "# how far the install got (Sims2Launcher.exe); delete this file to install again from the start\niso=" + s.iso + "\nversion=" +
                             s.version + "\nreached=" + std::to_string(static_cast<int>(s.reached)) + "\nerror=" + error + "\n";
    return ps2::os::replace_file(join(l.base, "install.ini"), text.data(), text.size());
}

std::uint64_t space_needed() { return std::uint64_t(3) << 30; }

std::string source_zip_url(const std::string &version) { return "https://github.com/gabe4tw/TheSims2-PS2-Recomp/archive/refs/tags/" + version + ".zip"; }

bool run_install(const Layout &l, InstallState &s, const InstallSetup &setup, const Report &report, const std::atomic<bool> &cancel) {
    std::error_code e;
    fs::create_directories(ps2::os::path(l.base), e);
    std::FILE *log = ps2::os::open(join(l.base, "install.log"), "ab");
    const auto say = [&](const std::string &line, double fraction) {
        if (log) {
            std::fprintf(log, "%s\n", line.c_str());
            std::fflush(log);
        }
        if (report) report(line, fraction);
    };
    const auto fail = [&](const std::string &why) {
        s.error = why;
        save_state(l, s);
        say("[install] failed: " + why, -1);
        if (log) std::fclose(log);
        return false;
    };
    const auto reached = [&](Step next) {
        s.reached = next;
        s.error.clear();
        save_state(l, s);
    };
    if (s.version.empty()) return fail("no version to install");

    if (s.reached <= Step::space) {
        const std::uint64_t free = setup.free_space ? setup.free_space(l.base) : free_bytes(l.base);
        if (free < space_needed()) {
            char text[160];
            std::snprintf(text, sizeof text, "Not enough free space on this drive: the install needs about %.0f GB, %.1f GB are free.",
                          static_cast<double>(space_needed()) / (1 << 30), static_cast<double>(free) / (1 << 30));
            return fail(text);
        }
        reached(Step::tools);
    }
    if (s.reached <= Step::tools) {
        for (std::size_t i = 0; i < setup.tools.size(); ++i) {
            const Download &t = setup.tools[i];
            const std::string marker = join(join(l.tools, t.folder), ".unpacked");
            if (exists_path(marker)) continue;
            say("[install] downloading " + t.name, 0.05 + 0.25 * static_cast<double>(i) / setup.tools.size());
            std::string why;
            if (!fetch_checked(t, l.downloads, [&](std::uint64_t, std::uint64_t) { return !cancel.load(); }, why)) return fail(cancel.load() ? "cancelled" : why);
            say("[install] unpacking " + t.name, 0.05 + 0.25 * (i + 0.5) / setup.tools.size());
            if (!unpack(t, join(l.downloads, download_file_name(t)), join(l.tools, t.folder), why)) return fail(why);
            ps2::os::replace_file(marker, "", 0);
        }
        reached(Step::source);
    }
    const std::string src = l.source(s.version);
    if (s.reached <= Step::source) {
        say("[install] downloading the remaster's source (" + s.version + ")", 0.32);
        const std::string zip = join(l.downloads, "source-" + s.version + ".zip");
        std::string why;
        fs::create_directories(ps2::os::path(l.downloads), e);
        if (!setup.source_url.empty() && exists_path(setup.source_url)) {
            fs::copy_file(ps2::os::path(setup.source_url), ps2::os::path(zip), fs::copy_options::overwrite_existing, e);
            if (e) return fail("cannot copy " + setup.source_url + " (" + e.message() + ")");
        } else {
            const std::string url = setup.source_url.empty() ? source_zip_url(s.version) : setup.source_url;
            if (!http_get(url, zip, [&](std::uint64_t, std::uint64_t) { return !cancel.load(); }, why)) return fail(cancel.load() ? "cancelled" : why);
        }
        const std::string unpack_dir = join(l.source_root, "_unpack-" + s.version);
        fs::remove_all(ps2::os::path(unpack_dir), e);
        Download zipped{"the source", "", "", 0, Download::Unpack::zip, ""};
        if (!unpack(zipped, zip, unpack_dir, why)) return fail(why);
        std::string top;
        int count = 0;
        for (fs::directory_iterator it(ps2::os::path(unpack_dir), e), end; !e && it != end; it.increment(e)) {
            ++count;
            top = ps2::os::utf8(it->path());
        }
        if (count != 1) return fail("the source zip does not have the expected single folder");
        if (exists_path(join(src, "games\\sims2")) && is_junction(join(src, "games\\sims2"))) remove_junction(join(src, "games\\sims2"));
        fs::remove_all(ps2::os::path(src), e);
        fs::rename(ps2::os::path(top), ps2::os::path(src), e);
        if (e) return fail("cannot move the source into " + src + " (" + e.message() + ")");
        fs::remove_all(ps2::os::path(unpack_dir), e);
        reached(Step::link);
    }
    if (s.reached <= Step::link) {
        fs::create_directories(ps2::os::path(l.data), e);
        fs::create_directories(ps2::os::path(src) / "games", e);
        const std::string link = join(src, "games\\sims2");
        if (!is_junction(link)) {
            std::error_code gone;
            if (fs::exists(ps2::os::path(link), gone)) fs::remove_all(ps2::os::path(link), gone);  // (a folder from the zip: the data folder wins)
            std::string why;
            if (!make_junction(link, l.data, why)) return fail(why);
        }
        // local change (stage 3): the HD textures belong to the game: downloaded with it, into the data folder (kept across updates)
        if (setup.hd_textures && !hdpack::installed(l.data)) {
            say("[install] downloading the HD textures (" + hdpack::size_text() + ")", 0.33);
            std::string file, why;
            if (!hdpack::fetch(l.downloads, setup.hd_local,
                               [&](std::uint64_t got, std::uint64_t total) {
                                   if (report && total)
                                       report("[install] downloading the HD textures: " + std::to_string(got >> 20) + " of " + std::to_string(total >> 20) + " MB",
                                              0.33 + 0.02 * static_cast<double>(got) / static_cast<double>(total));
                                   return !cancel.load();
                               },
                               file, why))
                return fail(cancel.load() ? "cancelled" : why);
            say("[install] unpacking the HD textures", 0.35);
            if (!hdpack::install(l.data, file, why)) return fail(why);
        }
        reached(Step::build);
    }
    if (s.reached <= Step::build) {
        const std::string bash = setup.bash.empty() ? ps2::os::utf8(ps2::os::path(l.tools) / "git" / "usr" / "bin" / "bash.exe") : setup.bash;
        const std::string path = setup.path.empty() ? tool_path(l.tools) : setup.path;
        int code = -1;
        std::string why, last_failure;
        const bool ran = run_with_output(
            bash, {"scripts/sims2_install.sh", s.iso}, src, path,
            [&](const std::string &line) {
                double fraction = -1;
                if (line.rfind("[install] step ", 0) == 0) fraction = 0.35 + 0.13 * (std::atoi(line.c_str() + 15) - 1);
                if (line.rfind("[install] failed: ", 0) == 0) last_failure = line.substr(18);
                say(line, fraction);
            },
            cancel, code, why);
        if (!ran) return fail(why);
        if (code != 0) return fail(last_failure.empty() ? "the build failed (code " + std::to_string(code) + "); see install.log in " + l.base : last_failure);
        if (!exists_path(ps2::os::utf8(ps2::os::path(src) / "build" / "native-sims2" / "ps2run.exe"))) return fail("the build ended without the game (ps2run.exe)");
        reached(Step::done);
    }
    say("[install] done", 1.0);
    if (log) std::fclose(log);
    return true;
}

bool run_update(const Layout &l, InstallState &s, const std::string &new_version, const InstallSetup &setup, const Report &report, const std::atomic<bool> &cancel) {
    InstallState u = s;
    u.version = new_version;
    u.reached = Step::source;
    u.error.clear();
    if (!run_install(l, u, setup, report, cancel)) {
        s.error = u.error;
        InstallState back = s;  // install.ini goes back to the version in use
        back.reached = Step::done;
        save_state(l, back);
        return false;
    }
    std::string why;
    if (!write_installed_config(l, u, why)) {
        s.error = why;
        return false;
    }
    s = u;
    return true;
}

bool write_installed_config(const Layout &l, const InstallState &s, std::string &why) {
    const std::string text = "# Sims2Launcher.exe settings, written by the install. Paths are relative to this folder.\nroot=source\\" + s.version + "\niso=" + s.iso +
                             "\ninstalled=" + s.version + "\n";
    if (!ps2::os::replace_file(join(l.base, "launcher.ini"), text.data(), text.size(), &why)) return false;
    return true;
}

std::string installed_version(const Layout &l) {
    std::FILE *f = ps2::os::open(join(l.base, "launcher.ini"), "rb");
    if (!f) return {};
    std::string version;
    char line[2048];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (t.rfind("installed=", 0) == 0) version = t.substr(10);
    }
    std::fclose(f);
    return version;
}

void remove_other_versions(const Layout &l, const std::string &keep) {
    std::error_code e;
    std::vector<fs::path> old;
    for (fs::directory_iterator it(ps2::os::path(l.source_root), e), end; !e && it != end; it.increment(e))
        if (ps2::os::utf8(it->path().filename()) != keep) old.push_back(it->path());
    for (const fs::path &dir : old) {
        const std::string link = ps2::os::utf8(dir / "games" / "sims2");
        if (is_junction(link) && !remove_junction(link)) continue;  // never delete through the link into the shared data
        if (is_junction(link)) continue;
        std::error_code gone;
        fs::remove_all(dir, gone);
    }
}
