// Local change (the remaster): the HD textures (see hd_pack.hpp).
#include "hd_pack.hpp"

#include "launcher_install.hpp"
#include "launcher_net.hpp"
#include "../runtime/src/gs/texpack.hpp"
#include "ps2/os.hpp"

#include <cctype>
#include <cstdio>
#include <filesystem>

namespace hdpack {

namespace fs = std::filesystem;

namespace {

std::string read_small(const std::string &file, std::size_t limit) {
    std::string text;
    if (std::FILE *f = ps2::os::open(file, "rb")) {
        char buf[4096];
        std::size_t n;
        while (text.size() < limit && (n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    return text;
}

// the value of attr="..." in a tag's text
std::string attribute(const std::string &tag, const std::string &name) {
    const std::string key = name + "=\"";
    std::size_t at = 0;
    while ((at = tag.find(key, at)) != std::string::npos) {
        if (at == 0 || tag[at - 1] == ' ' || tag[at - 1] == '\t' || tag[at - 1] == '\n') {
            const std::size_t from = at + key.size(), end = tag.find('"', from);
            if (end == std::string::npos) return {};
            std::string v = tag.substr(from, end - from);
            for (std::size_t amp; (amp = v.find("&amp;")) != std::string::npos;) v.replace(amp, 5, "&");
            return v;
        }
        at += key.size();
    }
    return {};
}

std::string url_escape(const std::string &v) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : v) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += '%', out += hex[c >> 4], out += hex[c & 15];
    }
    return out;
}

}  // namespace

std::string download_url(const Info &p) { return std::string("https://drive.usercontent.google.com/download?id=") + p.file_id + "&export=download&confirm=t"; }
std::string size_text(const Info &p) { return std::to_string((p.size + (1u << 19)) >> 20) + " MB"; }

std::string pictures_dir(const std::string &game_dir) { return ps2::os::utf8(ps2::os::path(game_dir) / "textures" / "hd"); }
std::string cache_dir(const std::string &game_dir) { return ps2::os::utf8(ps2::os::path(game_dir) / "textures" / ".hd-cache"); }

bool installed(const std::string &game_dir) {
    std::error_code e;
    for (auto it = fs::recursive_directory_iterator(ps2::os::path(pictures_dir(game_dir)), e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        std::string ext = ps2::os::utf8(it->path().extension());
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".dds" || ext == ".png") return true;
    }
    return false;
}

std::string drive_confirm_url(const std::string &html) {
    const std::size_t form = html.find("<form");
    if (form == std::string::npos) return {};
    const std::size_t form_end = html.find('>', form), close = html.find("</form>", form);
    if (form_end == std::string::npos) return {};
    std::string action = attribute(html.substr(form, form_end - form), "action");
    if (action.rfind("https://", 0) != 0) return {};
    std::string query;
    for (std::size_t at = form_end; (at = html.find("<input", at)) != std::string::npos && (close == std::string::npos || at < close);) {
        const std::size_t end = html.find('>', at);
        if (end == std::string::npos) break;
        const std::string tag = html.substr(at, end - at);
        const std::string name = attribute(tag, "name");
        if (!name.empty() && attribute(tag, "type") != "submit") query += (query.empty() ? "" : "&") + url_escape(name) + "=" + url_escape(attribute(tag, "value"));
        at = end;
    }
    return query.empty() ? action : action + (action.find('?') == std::string::npos ? "?" : "&") + query;
}

bool fetch(const std::string &downloads, const std::string &local_copy, const std::function<bool(std::uint64_t, std::uint64_t)> &progress, std::string &file,
           std::string &why, const Info &p) {
    std::error_code e;
    fs::create_directories(ps2::os::path(downloads), e);
    file = ps2::os::utf8(ps2::os::path(downloads) / p.archive_name);
    if (sha256_of_file(file) == p.sha256) return true;  // (downloaded before and still good)
    const std::string part = file + ".part";
    fs::remove(ps2::os::path(part), e);
    if (!local_copy.empty()) {
        fs::copy_file(ps2::os::path(local_copy), ps2::os::path(part), fs::copy_options::overwrite_existing, e);
        if (e) {
            why = "cannot copy " + local_copy + " (" + e.message() + ")";
            return false;
        }
    } else {
        std::string url = download_url(p);
        for (int attempt = 0;; ++attempt) {
            if (!http_get(url, part, progress, why)) return false;
            const std::uint64_t got = fs::file_size(ps2::os::path(part), e);
            if (e || got >= (2u << 20)) break;
            // a small answer is Google's page, not the archive: its form leads to the file
            const std::string next = drive_confirm_url(read_small(part, 1u << 20));
            fs::remove(ps2::os::path(part), e);
            if (next.empty() || next == url || attempt >= 2) {
                why = "Google Drive did not send the HD textures (it answered with a web page). Try again later, or download them from " + std::string(p.page) +
                      " and put " + p.archive_name + " into " + downloads + ".";
                return false;
            }
            url = next;
        }
    }
    if (sha256_of_file(part) != p.sha256) {
        fs::remove(ps2::os::path(part), e);
        why = "the downloaded HD textures are not the expected file (the checksum differs), so it was thrown away";
        return false;
    }
    fs::rename(ps2::os::path(part), ps2::os::path(file), e);
    if (e) {
        why = "cannot rename " + part + " (" + e.message() + ")";
        return false;
    }
    return true;
}

bool install(const std::string &game_dir, const std::string &archive, std::string &why) {
    std::error_code e;
    const fs::path textures = ps2::os::path(game_dir) / "textures", temp = textures / ".unpack-hd";
    fs::create_directories(textures, e);
    fs::remove_all(temp, e);
    Download d;
    d.name = "the HD textures";
    d.unpack = Download::Unpack::sevenzip;
    if (!unpack(d, archive, ps2::os::utf8(temp), why)) {
        fs::remove_all(temp, e);
        return false;
    }
    // the pictures' folder: the one that holds the .dds files (SLUS-21265\replacements\dds in the official archive)
    fs::path pictures;
    for (auto it = fs::recursive_directory_iterator(temp, e); !e && it != fs::recursive_directory_iterator(); it.increment(e)) {
        std::string ext = ps2::os::utf8(it->path().extension());
        for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".dds") {
            pictures = it->path().parent_path();
            break;
        }
    }
    if (pictures.empty()) {
        fs::remove_all(temp, e);
        why = "the HD textures archive holds no .dds pictures";
        return false;
    }
    const fs::path target = ps2::os::path(pictures_dir(game_dir));
    fs::remove_all(target, e);
    fs::rename(pictures, target, e);
    if (e) {
        why = "cannot move the HD textures into " + ps2::os::utf8(target) + " (" + e.message() + ")";
        std::error_code e2;
        fs::remove_all(temp, e2);
        return false;
    }
    fs::remove_all(temp, e);
    fs::remove(ps2::os::path(archive), e);  // (the pictures are in place now)
    // the game's index of the pictures (runtime/src/gs/texpack.hpp), made now: else the first game start makes it (seconds of original
    // textures); the game finds it in cache_dir and uses it as long as the pictures are unchanged
    fs::remove_all(ps2::os::path(cache_dir(game_dir)), e);
    ps2::texpack::Pack index(pictures_dir(game_dir), cache_dir(game_dir));
    index.scan();
    index.build_index();
    return true;
}

}  // namespace hdpack
