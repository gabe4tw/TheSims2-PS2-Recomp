#pragma once

// Local change (the remaster, launcher stage 3): mods, part 1. A mod is a folder in the mods folder (games\sims2\mods) with a mod.ini
// (name, author, version, game, description, url; "key = value" lines, # comments) and any of:
//   textures\     a PCSX2-style texture pack (found by picture in the game: runtime/src/gs/texpack.hpp)
//   patches.txt   memory patches in the runner's patch line format ("place address bytes value", as games\sims2\patches.txt)
//   settings.ini  PS2_NAME=value lines like remaster.ini; a mod's setting wins over remaster.ini
// Other files and folders are listed as ignored. A .zip in the mods folder is unpacked into a folder of its name by the launcher
// (the game never unpacks). mods.ini in the mods folder holds the order (highest priority first) and on / off:
//   on SomberTwilight HD
//   off My Test Mod
// Shared by the launcher (the Mods page) and the game (--mods <folder>: host/main.cpp), so both always agree.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mods {

constexpr const char *kGame = "SLES-53718";

struct Mod {
    std::string folder;  // its folder's name in the mods folder (the key in mods.ini)
    std::string path;    // the whole path
    std::string name, author, version, game, description, url;  // from mod.ini (name: the folder's when missing)
    bool enabled = false;
    bool has_ini = false;
    bool textures = false;  // has textures\ (with pictures in it or not)
    std::string patches_file, settings_file;  // "" = none
    std::vector<std::pair<std::string, std::string>> settings;  // PS2_NAME, value (file order)
    std::vector<std::uint32_t> patch_addresses;
    std::vector<std::string> ignored;  // files and folders the mod has that nothing reads
    bool wrong_game() const { return !game.empty() && game != kGame; }
    bool can_enable() const { return !wrong_game(); }
};

struct Library {
    std::string dir;          // the mods folder
    std::vector<Mod> mods;    // highest priority first
};

// Reads the mods folder: every folder in it is a mod (a folder starting with '.' is not); mods.ini gives order and on / off; a mod not
// in mods.ini yet goes on top, on (unless it is for another game); a line of a folder that is gone is dropped. Does not write anything.
// false (and why) only when the folder cannot be read; a missing folder is an empty library.
bool load(const std::string &dir, Library &out, std::string *why = nullptr);
// writes mods.ini (whole, or the old one stays)
bool save(const Library &lib, std::string *why = nullptr);

// what the launcher can do (each saves mods.ini)
bool set_enabled(Library &lib, int index, bool on, std::string *why = nullptr);
bool move(Library &lib, int index, int by, std::string *why = nullptr);  // by -1 = up (higher priority)
bool remove(Library &lib, int index, std::string *why = nullptr);        // deletes the folder (and its zip of the same name)

// The zips in the mods folder that have no folder of their name yet.
std::vector<std::string> new_zips(const std::string &dir);
// Unpacks `zip` into <dir>\<zip name without .zip>\ (a zip whose files are all inside one top folder with mod.ini gets that folder's
// contents). extract(zip, into, why) does the unpacking (the launcher: Windows' tar.exe). The new folder's name in `folder`.
using Extract = std::function<bool(const std::string &zip, const std::string &into, std::string &why)>;
bool install_zip(const std::string &dir, const std::string &zip, const Extract &extract, std::string &folder, std::string &why);

// Warnings for one mod against the other enabled mods: the same patch address, the same setting, the same texture pack picture, the
// same game texture matched in both packs (known after a game ran with both). Each line names the other mod and who wins.
std::vector<std::string> conflicts(const Library &lib, int index);

// What the game does with the enabled mods.
struct Plan {
    std::vector<std::string> patch_files;  // lowest priority first (the later patch wins)
    std::vector<std::pair<std::string, std::string>> settings;  // PS2_NAME -> value, each once: the highest mod's
    std::map<std::string, std::string> setting_owner;           // PS2_NAME -> the mod's name
    std::vector<std::pair<std::string, std::string>> texture_packs;  // (pictures folder, cache folder), highest priority first
    std::vector<std::string> names;  // the enabled mods' names, highest first
};
Plan plan(const Library &lib);
// PS2_MOD_SETTINGS: which settings a mod set ("PS2_A=Mod one;PS2_B=Mod two"), for the game window's Settings page
std::string owners_text(const std::map<std::string, std::string> &owners);
std::map<std::string, std::string> owners_from(const std::string &text);
// PS2_TEX_PACKS: "<pictures>*<cache>|..." (runtime/src/gs/gpu/gpu.cpp texrep)
std::string packs_text(const std::vector<std::pair<std::string, std::string>> &packs);

}  // namespace mods
