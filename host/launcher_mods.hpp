#pragma once

// Local change (the remaster, launcher stage 3): what the launcher's Mods page does besides the mod list itself (host/mod_library.hpp):
// the page's rows, the settings mods hold, zips dropped into the mods folder. No window here. (The HD textures are the game's own: hd_pack.hpp.)

#include "mod_library.hpp"
#include "quickmenu.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace launchermods {

// the Mods page's rows (each mod's line, details and warnings), made once per change: it lists the mods' folders
quickmenu::Values::Mods rows(const mods::Library &lib);
// The settings enabled mods hold, into remaster::g_options (now and next: what the game will use) and remaster::g_mod_owned (shown
// locked, kept out of remaster.ini). Call after remaster.ini was read.
void apply_settings(const mods::Library &lib);
// unpacks every .zip of the mods folder that has no folder yet (Windows' tar.exe); one line per zip for the status
std::vector<std::string> unpack_new_zips(const std::string &mods_dir);
// one zip chosen in the launcher: unpacked into the mods folder (a mod folder of that name must not exist yet)
bool install_zip(const std::string &mods_dir, const std::string &zip, std::string &folder, std::string &why);

}  // namespace launchermods
