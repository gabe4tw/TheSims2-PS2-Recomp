// Local change (the remaster, launcher stage 3): mods, part 1 (see mod_library.hpp).
#include "mod_library.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace mods {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    s.erase(0, i);
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) s.erase(0, 3);
    return s;
}

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::vector<std::string> lines_of(const std::string &path) {
    std::vector<std::string> out;
    std::FILE *f = ps2::os::open(path, "rb");
    if (!f) return out;
    char line[2048];
    while (std::fgets(line, sizeof line, f)) out.push_back(trim(line));
    std::fclose(f);
    return out;
}

bool write_text(const std::string &path, const std::string &text, std::string *why) {
    std::string err;
    if (ps2::os::replace_file(path, text.data(), text.size(), &err)) return true;
    if (why) *why = "cannot write " + path + (err.empty() ? "" : " (" + err + ")");
    return false;
}

void read_mod(Mod &m) {
    std::error_code e;
    const fs::path root = ps2::os::path(m.path);
    if (fs::is_regular_file(root / "mod.ini", e)) {
        m.has_ini = true;
        for (const std::string &line : lines_of(ps2::os::utf8(root / "mod.ini"))) {
            if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
            if (key == "name") m.name = value;
            else if (key == "author") m.author = value;
            else if (key == "version") m.version = value;
            else if (key == "game") m.game = value;
            else if (key == "description") m.description = value;
            else if (key == "url") m.url = value;
        }
    }
    if (m.name.empty()) m.name = m.folder;
    m.textures = fs::is_directory(root / "textures", e);
    if (fs::is_regular_file(root / "patches.txt", e)) {
        m.patches_file = ps2::os::utf8(root / "patches.txt");
        for (const std::string &line : lines_of(m.patches_file)) {
            unsigned place = 0, bytes = 0;
            unsigned long address = 0;
            unsigned long long value = 0;
            if (std::sscanf(line.c_str(), "%u %lx %u %llx", &place, &address, &bytes, &value) == 4) m.patch_addresses.push_back(static_cast<std::uint32_t>(address));
        }
    }
    if (fs::is_regular_file(root / "settings.ini", e)) {
        m.settings_file = ps2::os::utf8(root / "settings.ini");
        for (const std::string &line : lines_of(m.settings_file)) {
            const std::size_t eq = line.find('=');
            if (line.rfind("PS2_", 0) != 0 || eq == std::string::npos) continue;
            const std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (value.empty()) continue;
            bool again = false;
            for (auto &[n, v] : m.settings)
                if (n == name) v = value, again = true;  // (the last line of a name counts)
            if (!again) m.settings.emplace_back(name, value);
        }
    }
    for (const auto &entry : fs::directory_iterator(root, e)) {
        const std::string name = ps2::os::utf8(entry.path().filename()), low = lower(name);
        if (low == "mod.ini" || low == "textures" || low == "patches.txt" || low == "settings.ini" || low == ".cache") continue;
        if (low.rfind("readme", 0) == 0 || low.rfind("license", 0) == 0 || low.rfind("licence", 0) == 0) continue;
        m.ignored.push_back(name + (entry.is_directory(e) ? "\\" : ""));
    }
    std::sort(m.ignored.begin(), m.ignored.end());
}

// every picture file name of a pack (lower case, no folders)
std::unordered_set<std::string> picture_names(const Mod &m) {
    std::unordered_set<std::string> out;
    std::error_code e;
    const fs::path root = ps2::os::path(m.path) / "textures";
    for (auto it = fs::recursive_directory_iterator(root, e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        const std::string low = lower(ps2::os::utf8(it->path().filename()));
        if (low.size() > 4 && (low.compare(low.size() - 4, 4, ".dds") == 0 || low.compare(low.size() - 4, 4, ".png") == 0)) out.insert(low);
    }
    return out;
}

// our textures a pack has a picture for (its .cache\matches.txt, written by the game)
std::unordered_set<std::string> matched_textures(const Mod &m) {
    std::unordered_set<std::string> out;
    for (const std::string &line : lines_of(ps2::os::utf8(ps2::os::path(m.path) / ".cache" / "matches.txt"))) {
        const std::size_t space = line.find(' ');
        if (space == 16 && line.substr(space + 1) != "-") out.insert(line.substr(0, 16));
    }
    return out;
}

}  // namespace

bool load(const std::string &dir, Library &out, std::string *why) {
    out.dir = dir;
    out.mods.clear();
    std::error_code e;
    const fs::path root = ps2::os::path(dir);
    if (!fs::is_directory(root, e)) return true;
    std::vector<Mod> found;
    for (const auto &entry : fs::directory_iterator(root, e)) {
        std::error_code e2;
        if (!entry.is_directory(e2)) continue;
        Mod m;
        m.folder = ps2::os::utf8(entry.path().filename());
        if (m.folder.empty() || m.folder[0] == '.') continue;
        m.path = ps2::os::utf8(entry.path());
        read_mod(m);
        found.push_back(std::move(m));
    }
    if (e) {
        if (why) *why = "cannot read " + dir + " (" + e.message() + ")";
        return false;
    }
    std::sort(found.begin(), found.end(), [](const Mod &a, const Mod &b) { return lower(a.folder) < lower(b.folder); });
    // mods.ini: the listed ones in its order, the new ones before them
    std::vector<std::pair<std::string, bool>> listed;
    for (const std::string &line : lines_of(ps2::os::utf8(root / "mods.ini"))) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("on ", 0) == 0) listed.emplace_back(trim(line.substr(3)), true);
        else if (line.rfind("off ", 0) == 0) listed.emplace_back(trim(line.substr(4)), false);
    }
    std::vector<bool> placed(found.size(), false);
    std::vector<Mod> ordered;
    for (std::size_t i = 0; i < found.size(); ++i) {
        bool in_list = false;
        for (const auto &[folder, on] : listed) in_list = in_list || folder == found[i].folder;
        if (!in_list) {
            found[i].enabled = found[i].can_enable();
            ordered.push_back(found[i]);
            placed[i] = true;
        }
    }
    for (const auto &[folder, on] : listed)
        for (std::size_t i = 0; i < found.size(); ++i)
            if (!placed[i] && found[i].folder == folder) {
                found[i].enabled = on && found[i].can_enable();
                ordered.push_back(found[i]);
                placed[i] = true;
            }
    out.mods = std::move(ordered);
    return true;
}

bool save(const Library &lib, std::string *why) {
    std::error_code e;
    fs::create_directories(ps2::os::path(lib.dir), e);
    std::string text = "# Mods (Sims2Launcher.exe's Mods page): highest priority first; on / off and the mod's folder\n";
    for (const Mod &m : lib.mods) text += (m.enabled ? "on " : "off ") + m.folder + "\n";
    return write_text(ps2::os::utf8(ps2::os::path(lib.dir) / "mods.ini"), text, why);
}

bool set_enabled(Library &lib, int index, bool on, std::string *why) {
    if (index < 0 || index >= static_cast<int>(lib.mods.size())) return false;
    Mod &m = lib.mods[static_cast<std::size_t>(index)];
    if (on && !m.can_enable()) {
        if (why) *why = "\"" + m.name + "\" is made for " + m.game + ", not this game (" + kGame + ").";
        return false;
    }
    m.enabled = on;
    return save(lib, why);
}

bool move(Library &lib, int index, int by, std::string *why) {
    const int to = index + by, n = static_cast<int>(lib.mods.size());
    if (index < 0 || index >= n || to < 0 || to >= n) return false;
    std::swap(lib.mods[static_cast<std::size_t>(index)], lib.mods[static_cast<std::size_t>(to)]);
    return save(lib, why);
}

bool remove(Library &lib, int index, std::string *why) {
    if (index < 0 || index >= static_cast<int>(lib.mods.size())) return false;
    const Mod m = lib.mods[static_cast<std::size_t>(index)];
    std::error_code e;
    fs::remove_all(ps2::os::path(m.path), e);
    if (e || fs::exists(ps2::os::path(m.path))) {
        if (why) *why = "cannot delete " + m.path + (e ? " (" + e.message() + ")" : "");
        return false;
    }
    fs::remove(ps2::os::path(lib.dir) / ps2::os::path(m.folder + ".zip"), e);  // else it would come back as a new mod
    lib.mods.erase(lib.mods.begin() + index);
    return save(lib, why);
}

std::vector<std::string> new_zips(const std::string &dir) {
    std::vector<std::string> out;
    std::error_code e;
    const fs::path root = ps2::os::path(dir);
    for (const auto &entry : fs::directory_iterator(root, e)) {
        const std::string name = ps2::os::utf8(entry.path().filename());
        if (name.size() <= 4 || lower(name.substr(name.size() - 4)) != ".zip") continue;
        std::error_code e2;
        if (!fs::exists(root / ps2::os::path(name.substr(0, name.size() - 4)), e2)) out.push_back(ps2::os::utf8(entry.path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool install_zip(const std::string &dir, const std::string &zip, const Extract &extract, std::string &folder, std::string &why) {
    std::error_code e;
    const fs::path root = ps2::os::path(dir), zip_path = ps2::os::path(zip);
    std::string base = ps2::os::utf8(zip_path.filename());
    if (base.size() > 4 && lower(base.substr(base.size() - 4)) == ".zip") base.resize(base.size() - 4);
    if (base.empty() || base[0] == '.') base = "mod" + base;
    fs::create_directories(root, e);
    folder = base;
    if (fs::exists(root / ps2::os::path(folder), e)) {
        why = "a mod folder \"" + folder + "\" is there already";
        return false;
    }
    const fs::path temp = root / ps2::os::path(".unpack-" + folder);
    fs::remove_all(temp, e);
    fs::create_directories(temp, e);
    if (!extract(zip, ps2::os::utf8(temp), why)) {
        fs::remove_all(temp, e);
        if (why.empty()) why = "cannot unpack " + zip;
        return false;
    }
    // the mod's own files: at the top, or inside one top folder
    fs::path source = temp;
    if (!fs::exists(temp / "mod.ini", e)) {
        std::vector<fs::path> entries;
        for (const auto &entry : fs::directory_iterator(temp, e)) entries.push_back(entry.path());
        if (entries.size() == 1 && fs::is_directory(entries[0], e)) source = entries[0];
    }
    if (!fs::exists(source / "mod.ini", e) && !fs::exists(source / "textures", e) && !fs::exists(source / "patches.txt", e) && !fs::exists(source / "settings.ini", e)) {
        fs::remove_all(temp, e);
        why = "the zip has no mod.ini, textures, patches.txt or settings.ini: it is not a mod";
        return false;
    }
    fs::rename(source, root / ps2::os::path(folder), e);
    if (e) {
        why = "cannot move the unpacked mod into place (" + e.message() + ")";
        fs::remove_all(temp, e);
        return false;
    }
    fs::remove_all(temp, e);
    return true;
}

std::vector<std::string> conflicts(const Library &lib, int index) {
    std::vector<std::string> out;
    if (index < 0 || index >= static_cast<int>(lib.mods.size())) return out;
    const Mod &m = lib.mods[static_cast<std::size_t>(index)];
    std::unordered_set<std::string> pictures, matched;
    bool pictures_read = false;
    for (int i = 0; i < static_cast<int>(lib.mods.size()); ++i) {
        const Mod &o = lib.mods[static_cast<std::size_t>(i)];
        if (i == index || !o.enabled) continue;
        const std::string winner = i < index ? o.name : m.name;
        const std::string tail = " (\"" + winner + "\" wins: it is higher)";
        int same = 0;
        for (std::uint32_t a : m.patch_addresses) same += std::count(o.patch_addresses.begin(), o.patch_addresses.end(), a) > 0;
        if (same) out.push_back("Patches: " + std::to_string(same) + " address" + (same == 1 ? "" : "es") + " also patched by \"" + o.name + "\"" + tail);
        for (const auto &[name, value] : m.settings)
            for (const auto &[oname, ovalue] : o.settings)
                if (name == oname) out.push_back("Setting " + name + " also set by \"" + o.name + "\"" + tail);
        if (m.textures && o.textures) {
            if (!pictures_read) pictures = picture_names(m), matched = matched_textures(m), pictures_read = true;
            int names = 0, games = 0;
            for (const std::string &n : picture_names(o)) names += pictures.count(n) > 0;
            for (const std::string &t : matched_textures(o)) games += matched.count(t) > 0;
            if (names) out.push_back("Textures: " + std::to_string(names) + " picture" + (names == 1 ? "" : "s") + " also in \"" + o.name + "\"" + tail);
            if (games) out.push_back("Textures: " + std::to_string(games) + " game texture" + (games == 1 ? "" : "s") + " also replaced by \"" + o.name + "\"" + tail);
        }
    }
    return out;
}

Plan plan(const Library &lib) {
    Plan p;
    for (const Mod &m : lib.mods) {
        if (!m.enabled) continue;
        p.names.push_back(m.name);
        for (const auto &[name, value] : m.settings)
            if (!p.setting_owner.count(name)) {
                p.setting_owner[name] = m.name;
                p.settings.emplace_back(name, value);
            }
        if (m.textures) p.texture_packs.emplace_back(ps2::os::utf8(ps2::os::path(m.path) / "textures"), ps2::os::utf8(ps2::os::path(m.path) / ".cache"));
    }
    for (auto it = lib.mods.rbegin(); it != lib.mods.rend(); ++it)
        if (it->enabled && !it->patches_file.empty()) p.patch_files.push_back(it->patches_file);
    return p;
}

std::string owners_text(const std::map<std::string, std::string> &owners) {
    std::string out;
    for (const auto &[name, mod] : owners) {
        std::string clean = mod;
        std::replace(clean.begin(), clean.end(), ';', ',');
        out += (out.empty() ? "" : ";") + name + "=" + clean;
    }
    return out;
}

std::map<std::string, std::string> owners_from(const std::string &text) {
    std::map<std::string, std::string> out;
    std::size_t from = 0;
    while (from < text.size()) {
        std::size_t end = text.find(';', from);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(from, end - from);
        const std::size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) out[item.substr(0, eq)] = item.substr(eq + 1);
        from = end + 1;
    }
    return out;
}

std::string packs_text(const std::vector<std::pair<std::string, std::string>> &packs) {
    std::string out;
    for (const auto &[pictures, cache] : packs) out += (out.empty() ? "" : "|") + pictures + "*" + cache;
    return out;
}

}  // namespace mods
