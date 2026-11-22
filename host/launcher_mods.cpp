// Local change (the remaster, launcher stage 3): the launcher's Mods page helpers (see launcher_mods.hpp).
#include "launcher_mods.hpp"

#include "launcher_install.hpp"
#include "launcher_net.hpp"
#include "remaster_ini.hpp"
#include "ps2/os.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace launchermods {

namespace fs = std::filesystem;

namespace {

std::size_t count_pictures(const std::string &mod_path) {
    std::size_t n = 0;
    std::error_code e;
    for (auto it = fs::recursive_directory_iterator(ps2::os::path(mod_path) / "textures", e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        std::string ext = ps2::os::utf8(it->path().extension());
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        n += ext == ".dds" || ext == ".png";
    }
    return n;
}

bool extract_with_tar(const std::string &zip, const std::string &into, std::string &why) {
    Download d;
    d.name = ps2::os::utf8(ps2::os::path(zip).filename());
    d.unpack = Download::Unpack::zip;
    return unpack(d, zip, into, why);
}

}  // namespace

quickmenu::Values::Mods rows(const mods::Library &lib) {
    quickmenu::Values::Mods out;
    for (int i = 0; i < static_cast<int>(lib.mods.size()); ++i) {
        const mods::Mod &m = lib.mods[static_cast<std::size_t>(i)];
        quickmenu::Values::ModRow r;
        r.name = m.name;
        r.on = m.enabled;
        r.can_on = m.can_enable();
        if (!m.author.empty()) r.info = "by " + m.author;
        if (!m.version.empty()) r.info += (r.info.empty() ? "version " : ", version ") + m.version;
        std::string has;
        if (m.textures) has += "textures (" + std::to_string(count_pictures(m.path)) + " pictures)";
        if (!m.patches_file.empty()) has += (has.empty() ? "" : ", ") + std::to_string(m.patch_addresses.size()) + " patches";
        if (!m.settings.empty()) {
            std::string names;
            for (const auto &[n, v] : m.settings) names += (names.empty() ? "" : ", ") + n + "=" + v;
            has += (has.empty() ? "settings: " : "; settings: ") + names;
        }
        r.details = (m.description.empty() ? std::string() : m.description + "\n") + "Has: " + (has.empty() ? std::string("nothing the game uses") : has) + ".";
        if (!m.has_ini) r.details += " (No mod.ini: named after its folder.)";
        if (!m.ignored.empty()) {
            std::string list;
            for (const std::string &x : m.ignored) list += (list.empty() ? "" : ", ") + x;
            r.details += " Ignored: " + list + ".";
        }
        if (m.wrong_game()) r.warnings.push_back("Made for " + m.game + ", not this game (" + mods::kGame + "): it cannot be turned on.");
        if (m.enabled)
            for (const std::string &w : mods::conflicts(lib, i)) r.warnings.push_back(w);
        out.rows.push_back(std::move(r));
    }
    return out;
}

void apply_settings(const mods::Library &lib) {
    const mods::Plan plan = mods::plan(lib);
    remaster::g_mod_owned = plan.setting_owner;
    for (int i = 0; i < remaster::kOptionCount; ++i) {
        remaster::Option &o = remaster::g_options[i];
        for (const auto &[name, value] : plan.settings)
            if (name == o.variable) {
                int v = std::atoi(value.c_str());
                if (i == 1 && v == 1) v = 75;  // (PS2_UI_SQUEEZE=1 is the true shape, as remaster.ini reads it)
                o.now = o.next = v;
            }
    }
}

std::vector<std::string> unpack_new_zips(const std::string &mods_dir) {
    std::vector<std::string> lines;
    for (const std::string &zip : mods::new_zips(mods_dir)) {
        std::string folder, why;
        if (mods::install_zip(mods_dir, zip, extract_with_tar, folder, why)) lines.push_back("Unpacked " + ps2::os::utf8(ps2::os::path(zip).filename()) + " as a new mod.");
        else lines.push_back(ps2::os::utf8(ps2::os::path(zip).filename()) + ": " + why);
    }
    return lines;
}

bool install_zip(const std::string &mods_dir, const std::string &zip, std::string &folder, std::string &why) {
    return mods::install_zip(mods_dir, zip, extract_with_tar, folder, why);
}

}  // namespace launchermods
