#pragma once

// Local change (the remaster): PCSX2-style texture packs, found by picture (launcher stage 3). A pack is a folder (searched with its
// subfolders) of pictures named <texture hash>-<palette hash>-<flags>.dds or .png, as PCSX2 dumps and loads them; the flags carry the
// texture's native size (tw = (flags >> 6) & 15, th = (flags >> 10) & 15: 2^tw x 2^th). Our textures are named by another number
// (gpu.cpp texrep: a hash of the decoded picture), so a pack picture is found by looking: every pack picture is shrunk to its native
// size once (the index, kept in <cache>/thumbs.bin), and a texture of our game is compared with the pack pictures of its size; the
// least different one under a threshold is its larger picture. The comparison is the one of C:\PS2Native\tex_match.py (the 200
// hand-checked matches of textures\enhanced\matches.txt were made with it), so the same pictures are chosen.
// Pixels are RGBA words: R | G << 8 | B << 16 | A << 24, the alpha as stored (the console's: 0x80 = opaque). CPU only, no graphics.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2::texpack {

struct Name {
    std::uint64_t texture = 0, palette = 0;
    std::uint32_t flags = 0;
    int tw = 0, th = 0;  // native size: 2^tw x 2^th
};
// "10217bf7876d896b-f14b391ae9fffdb0-00005553.dds" (any case, .dds or .png); false for anything else
bool parse_name(const std::string &file_name, Name &out);

// a picture file (.dds: DXT1/3/5 and plain 24/32-bit; .png: 8 / 16-bit grey, RGB, palette, grey + alpha, RGBA, not interlaced)
bool load_picture(const std::string &path, std::vector<std::uint32_t> &out, std::uint32_t &w, std::uint32_t &h, std::string *why = nullptr);
bool decode_dds(const std::uint8_t *data, std::size_t size, std::vector<std::uint32_t> &out, std::uint32_t &w, std::uint32_t &h, std::string *why = nullptr);
bool decode_png(const std::uint8_t *data, std::size_t size, std::vector<std::uint32_t> &out, std::uint32_t &w, std::uint32_t &h, std::string *why = nullptr);
// zlib's inflate (raw deflate blocks after the 2-byte zlib header); false on a broken stream
bool inflate_zlib(const std::uint8_t *data, std::size_t size, std::vector<std::uint8_t> &out);

// the picture shrunk to dw x dh as Pillow's resize(BOX) of an RGBA picture does it (alpha premultiplied, rows then columns, each
// rounded to bytes, then unpremultiplied): the same thumbnails as tex_match.py
void shrink(const std::uint32_t *src, std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh, std::vector<std::uint32_t> &out);
// tex_match.py's difference of two pictures of the same size: colour difference weighted by min(1, max(alpha) / 128) plus half the
// alpha difference (0 = the same; a match is under 9)
double difference(const std::uint32_t *a, const std::uint32_t *b, std::size_t count);
// a picture of nearly one colour (all channels within 2): such a texture is never matched (it would match any plain pack picture)
bool is_plain(const std::uint32_t *pixels, std::size_t count);

constexpr double kThreshold = 9.0;

// One pack: its files, its thumbnails (the index) and what was found for our textures (the match cache).
class Pack {
public:
    // folder: the pictures; cache: where thumbs.bin and matches.txt are kept ("" = nothing kept)
    Pack(std::string folder, std::string cache);
    const std::string &folder() const { return folder_; }
    const std::string &cache_dir() const { return cache_dir_; }
    // lists the pictures and reads the match cache (cheap)
    void scan();
    std::size_t file_count() const { return files_.size(); }
    // the thumbnails: from thumbs.bin when it fits the files, else made (decodes every picture; seconds) and written there.
    // stop() returning true ends it early (the pack is then not ready). Safe to call from a thread other than match()'s callers
    // only before ready() is true.
    void build_index(const std::function<bool()> &stop = {});
    bool ready() const { return ready_; }
    struct Result {
        int file = -1;      // the chosen picture (index of path()), -1 = none under the threshold
        double diff = 1e9;  // its difference
    };
    // the least different picture made for a texture of this size (needs ready())
    Result match(const std::uint32_t *pixels, std::uint32_t w, std::uint32_t h, double threshold = kThreshold) const;
    const std::string &path(int file) const { return files_[static_cast<std::size_t>(file)].path; }
    const std::string &file_name(int file) const { return files_[static_cast<std::size_t>(file)].name; }
    // the match cache: our hash -> file index or -1 (looked at, nothing); found = false when not looked at yet
    int cached(std::uint64_t ours, bool &found) const;
    void remember(std::uint64_t ours, int file);  // also appended to matches.txt
    std::size_t cached_count() const;
    std::uint64_t index_ms = 0;  // how long the last build_index took
    bool index_from_file = false;

private:
    struct File {
        std::string path, name;  // name: relative to the folder, '/' separated
        std::uint64_t size = 0;
        std::int64_t time = 0;
        int tw = 0, th = 0;
    };
    std::string folder_, cache_dir_;
    std::vector<File> files_;
    std::unordered_map<std::string, int> by_name_;
    // per native size (tw | th << 4): the files and their thumbnails one after another
    struct Bucket {
        std::vector<int> files;
        std::vector<std::uint32_t> pixels;
    };
    std::unordered_map<int, Bucket> buckets_;
    bool ready_ = false;
    mutable std::mutex cache_mutex_;
    std::unordered_map<std::uint64_t, int> cache_;
    bool read_thumbs();
    void write_thumbs() const;
};

}  // namespace ps2::texpack
