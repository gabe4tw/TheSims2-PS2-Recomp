#pragma once

// Local change (the remaster): the HD textures - SomberTwilight's texture pack (made for the US disc, used with the author's permission),
// part of the game, not a mod. The install downloads it by default into the game's data folder (<game_dir>\textures\hd, the shared
// data folder of a player's install, so it survives updates); the game finds its pictures by picture (runtime/src/gs/texpack.hpp) and
// the Settings option "Textures: Original / Updated (HD)" switches it. Mods' texture packs come before it, the old textures\enhanced
// folder after it.

#include <cstdint>
#include <functional>
#include <string>

namespace hdpack {

struct Info {
    const char *file_id = "1nTJ07wGYAB1MyYHGEXiSBsCeVTeN2QGY";  // its official Google Drive file
    const char *sha256 = "3015e25ae8a917b112f6b277bc301023d16d1986b02e90fbc5660b24642ed7f5";
    std::uint64_t size = 102485466;
    const char *archive_name = "SLUS-21265.7z";
    const char *page = "https://drive.google.com/file/d/1nTJ07wGYAB1MyYHGEXiSBsCeVTeN2QGY/view";
};
std::string download_url(const Info &p = {});
std::string size_text(const Info &p = {});  // "98 MB"

std::string pictures_dir(const std::string &game_dir);  // <game_dir>\textures\hd
std::string cache_dir(const std::string &game_dir);     // <game_dir>\textures\.hd-cache (the game's index and answers)
bool installed(const std::string &game_dir);            // the pictures' folder has pictures

// Google Drive answers a large file's download with a page ("can't scan this file for viruses") holding a form whose action and
// hidden fields make the real download link: that link, or "" when the page has no such form.
std::string drive_confirm_url(const std::string &html);
// The archive into <downloads>\SLUS-21265.7z, checked against its sha256 (a file there already that checks is kept, no request).
// local_copy: copy that file instead of downloading (tests: PS2_LAUNCHER_HDPACK). progress(got, total) false = cancel.
bool fetch(const std::string &downloads, const std::string &local_copy, const std::function<bool(std::uint64_t, std::uint64_t)> &progress, std::string &file,
           std::string &why, const Info &p = {});
// Unpacks the archive (Windows' tar.exe) and puts its pictures into pictures_dir(game_dir) (one there before is replaced); the archive
// is deleted after.
bool install(const std::string &game_dir, const std::string &archive, std::string &why);

}  // namespace hdpack
