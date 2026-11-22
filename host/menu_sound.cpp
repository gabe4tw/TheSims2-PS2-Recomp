#include "menu_sound.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace menusound {
namespace {

std::vector<std::int16_t> g_clips[static_cast<int>(Clip::count_)];  // stereo frames, filled once before any play
std::atomic<int> g_playing{-1};      // the clip, or -1
std::atomic<std::uint32_t> g_start{0};  // bumped by play(): the mixer starts the clip from its beginning
bool g_on = true;
constexpr int kVolume = 160;  // of 256: a little under the game's own level

std::uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
std::uint16_t le16(const unsigned char *p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

// a RIFF WAV of 16-bit PCM, 48000 Hz, stereo (what extract_ui_sounds.py writes); anything else is refused
bool read_wav(const std::string &path, std::vector<std::int16_t> &out) {
    std::FILE *f = ps2::os::open(path, "rb");
    if (!f) return false;
    std::vector<unsigned char> d;
    unsigned char buffer[65536];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) d.insert(d.end(), buffer, buffer + n);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return false;
    bool format_ok = false;
    for (std::size_t at = 12; at + 8 <= d.size();) {
        const std::uint32_t size = le32(&d[at + 4]);
        const std::size_t body = at + 8;
        if (body + size > d.size()) break;
        if (std::memcmp(&d[at], "fmt ", 4) == 0 && size >= 16)
            format_ok = le16(&d[body]) == 1 && le16(&d[body + 2]) == 2 && le32(&d[body + 4]) == 48000 && le16(&d[body + 14]) == 16;
        if (std::memcmp(&d[at], "data", 4) == 0 && format_ok) {
            out.resize(size / 2);
            std::memcpy(out.data(), &d[body], out.size() * 2);
            return !out.empty();
        }
        at = body + size + (size & 1);
    }
    return false;
}

}  // namespace

bool load(const std::string &folder) {
    g_on = ps2::os::env("PS2_MENU_SOUNDS") != "0";
    static const char *const kNames[] = {"move.wav", "select.wav", "back.wav"};
    int found = 0;
    for (int i = 0; i < static_cast<int>(Clip::count_); ++i)
        if (read_wav(folder + "/" + kNames[i], g_clips[i])) ++found;
    std::fprintf(stderr, "[menu] %d of 3 menu sounds read from %s%s\n", found, folder.c_str(), g_on ? "" : " (off: PS2_MENU_SOUNDS=0)");
    return found > 0;
}

void play(Clip clip) {
    const int i = static_cast<int>(clip);
    if (!g_on || i < 0 || i >= static_cast<int>(Clip::count_) || g_clips[i].empty()) return;
    g_playing.store(i, std::memory_order_relaxed);
    g_start.fetch_add(1, std::memory_order_release);
}

void mix(std::int16_t *stereo, std::size_t frames) {
    static std::uint32_t seen = 0;
    static std::size_t position = 0;  // in samples (two per frame)
    static int clip = -1;
    const std::uint32_t start = g_start.load(std::memory_order_acquire);
    if (start != seen) {
        seen = start;
        clip = g_playing.load(std::memory_order_relaxed);
        position = 0;
    }
    if (clip < 0) return;
    const std::vector<std::int16_t> &c = g_clips[clip];
    const std::size_t n = std::min(frames * 2, c.size() - std::min(position, c.size()));
    for (std::size_t i = 0; i < n; ++i) {
        const int v = stereo[i] + c[position + i] * kVolume / 256;
        stereo[i] = static_cast<std::int16_t>(std::clamp(v, -32768, 32767));
    }
    position += n;
    if (position >= c.size()) clip = -1;
}

}  // namespace menusound
