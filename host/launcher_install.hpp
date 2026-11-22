#pragma once

// Local change (the remaster): the launcher's install logic (stage 2): which disc this is, and (later tasks) downloads, the folder
// layout and the install steps. Plan: C:\PS2Native\plans\2026-11-17-launcher-stage2-plan.md.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class DiscKind { sles53718, slus21265, other, unreadable };
// reads SYSTEM.CNF's BOOT2 from the disc image (any format the runtime opens); boot_name = "SLES_537.18" etc.
DiscKind check_disc(const std::string &iso, std::string &boot_name, std::string &why);
// the line a player sees for each kind ("" for the right disc)
std::string disc_message(DiscKind kind);
// the *.iso files in a folder (any case), sorted by name
std::vector<std::string> find_isos(const std::string &dir);

// ---- downloads ----
// progress(got bytes, total bytes or 0); returning false cancels
using Progress = std::function<bool(std::uint64_t got, std::uint64_t total)>;
struct Download {
    std::string name, url, sha256;  // sha256: lower-case hex
    std::uint64_t size = 0;
    enum class Unpack { zip, sevenzip, portable_git, none } unpack = Unpack::none;
    std::string folder;             // under tools\ (or the target folder of unpack)
};
// llvm-mingw, cmake, ninja, PortableGit, Python embeddable: fixed versions, sizes and sha256 (pinned 2026-11-17)
const std::vector<Download> &tool_downloads();
std::string sha256_file(const std::string &file);  // "" when the file cannot be read
// <dir>\<file name of the URL>: kept when a file with the right sha256 is there already (no request); else downloaded as .part, checked,
// renamed. A file that fails the check is deleted, never kept.
bool fetch_checked(const Download &d, const std::string &dir, const Progress &progress, std::string &why);
std::string download_file_name(const Download &d);
// unpacks into `into` (made if missing): zip and 7z with Windows' own tar.exe, PortableGit by running its self-extractor
bool unpack(const Download &d, const std::string &file, const std::string &into, std::string &why);
// the PATH for the build: the downloaded tools' program folders under tools_dir, then C:\Windows\System32 - nothing else
std::string tool_path(const std::string &tools_dir);

// ---- the install ----
// the player's folder: <base>\launcher.ini, tools\, downloads\, source\<version>\ (its games\sims2 is a junction to data\sims2: cards,
// settings, states, recordings are shared by every version and survive updates), install.ini (how far the install got), install.log
struct Layout {
    std::string base, tools, downloads, data, source_root;
    std::string source(const std::string &version) const;
};
Layout layout_for(const std::string &base);
enum class Step { space = 0, tools, source, link, build, done };
struct InstallState {
    std::string iso, version;
    Step reached = Step::space;
    std::string error;
};
bool load_state(const Layout &l, InstallState &s);
bool save_state(const Layout &l, const InstallState &s);
std::uint64_t space_needed();  // bytes the install needs free (tools + downloads + source + build, measured 2026-11-17: ~2 GB; asks 3)
// what the steps use; the defaults are the real ones, tests replace them
struct InstallSetup {
    std::vector<Download> tools = tool_downloads();
    std::string source_url;  // "" = the repo's tag zip on GitHub; an existing local file is copied instead
    std::string bash;        // "" = <tools>\git\usr\bin\bash.exe
    std::string path;        // "" = tool_path(<tools>)
    std::function<std::uint64_t(const std::string &dir)> free_space;  // empty = the drive's real free space
    // local change (stage 3): the HD textures (host/hd_pack.hpp) are downloaded into the data folder in the data-folder step (the
    // launcher always sets this; an update gets them too when they are missing). hd_local: a local archive instead (PS2_LAUNCHER_HDPACK)
    bool hd_textures = false;
    std::string hd_local;
};
// the GitHub zip of a release tag of the remaster
std::string source_zip_url(const std::string &version);
using Report = std::function<void(const std::string &line, double fraction)>;
// Runs every step from s.reached on (a step done is not done again), saving install.ini after each; false with s.error set.
bool run_install(const Layout &l, InstallState &s, const InstallSetup &setup, const Report &report, const std::atomic<bool> &cancel);
// An update: the new version is built into source\<new> (tools and data reused); launcher.ini switches to it only after its build
// succeeded, so a failed update leaves the old version playable.
bool run_update(const Layout &l, InstallState &s, const std::string &new_version, const InstallSetup &setup, const Report &report, const std::atomic<bool> &cancel);
// launcher.ini for an installed version: root=source\<version>, iso=, installed=<version>
bool write_installed_config(const Layout &l, const InstallState &s, std::string &why);
std::string installed_version(const Layout &l);  // "" when launcher.ini has no installed= line
// deletes every source\<version> but `keep` (the junction to data\sims2 is removed first, so the shared data stays)
void remove_other_versions(const Layout &l, const std::string &keep);
