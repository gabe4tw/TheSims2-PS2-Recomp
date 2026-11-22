#include "gs.hpp"

#include "gif.hpp"
#include "queue.hpp"
#include "raster.hpp"
#include "../hw.hpp"
#include "../kernel.hpp"
#ifdef PS2_GPU
#include "gpu/gpu.hpp"
#endif

#include "ps2/hitch.hpp"
#include "ps2/os.hpp"
#include "ps2/state.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <functional>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps2::vu { extern std::uint32_t g_trace_vu1_pc; void trace_dump(const char *path); }

namespace ps2::gs {

namespace trace {  // defined further down (PS2_TRACE_DRAWS)
extern std::uint64_t g_frame, g_number;
bool traced(std::uint64_t frame);
extern const bool g_any;  // PS2_TRACE_DRAWS is set: kick() looks at this flag instead of calling into the trace code for every shape
}

State g_state;
std::uint32_t g_setup_generation = 0;

void serve_state(const void *payload);

namespace {

alignas(64) std::uint8_t g_memory[kMemorySize];

std::uint64_t g_pmode, g_smode1, g_smode2, g_srfsh, g_synch1, g_synch2, g_syncv;
std::uint64_t g_dispfb[2], g_display[2], g_extbuf, g_extdata, g_extwrite, g_bgcolor;
std::uint64_t g_csr = 0x551B4000ull, g_imr = 0x7F00ull, g_busdir, g_siglblid;
std::uint32_t g_finish_pending = 0;  // FINISH events that arrived while CSR.FINISH was still set; raised as the game clears the bit
std::uint32_t g_finish_queued = 0;   // FINISH writes that came while an earlier one was still on its way; each gets its own event (not in a state)
std::uint64_t g_primitives = 0;
constexpr std::uint64_t kFinishDelay = 1000; // EE cycles from the FINISH write to CSR.FINISH

#ifdef PS2_GPU
bool gpu_on() { return gpu::mode() != gpu::Mode::off; }
bool cpu_raster() { return gpu::mode() != gpu::Mode::primary; }
#else
constexpr bool gpu_on() { return false; }
constexpr bool cpu_raster() { return true; }
#endif

bool trace_display() {
    static const bool enabled = std::getenv("PS2_TRACE_DISPLAY") != nullptr;
    return enabled;
}
// The lines of the picture the video mode displays: DH+1 lines at most, no more than the mode has (PAL 288 a field, NTSC 240; twice
// that interlaced), and fewer when the game moves the display rectangle down. DY counts lines from the top of the frame and the mode's
// first line is `top` (36 PAL, 25 NTSC; doubled interlaced): a rectangle that starts `below` lines under it loses that many lines at
// the bottom of the TV picture. Follows upstream a75fcdd and fe11596 (snuri00/ps2-web-recomp).
// Only the TV modes have those limits: NTSC (2) and PAL (3) of SetGsCrt, and 0 when it was never called. Progressive scan (480p 0x50),
// 1080i, 720p and VGA (the mode values of ps2sdk's graph library) show DH+1 lines whatever DY says, so a game moved to 480p by a
// no-interlacing patch is not cut to half of its height (upstream has this fault).
std::uint32_t shown_lines(std::uint32_t dh, std::uint32_t dy, std::uint64_t smode2) {
    const std::uint32_t mode = hw::crt_mode();
    if (mode != 0u && mode != 2u && mode != 3u) return dh;
    const std::uint32_t scale = (smode2 & 1u) ? 2u : 1u;
    const std::uint32_t lines = (ps2::g_options.pal ? 288u : 240u) * scale;
    const std::uint32_t top = (ps2::g_options.pal ? 36u : 25u) * scale, span = (ps2::g_options.pal ? 256u : 224u) * scale;
    const std::uint32_t below = dy > top ? std::min(dy - top, span) : 0u;
    return std::min({dh, lines, below ? span - below : lines});
}
// Height of the displayed picture in lines. The display rectangle's height DH+1 counts lines of the video frame, and a TV picture
// cannot have more than 576 of them (PAL; NTSC has 480). A game in interlaced field mode (SMODE2 INT=1, FFMD=0) that still asks for
// more (Silent Hill 2 sets 896 for a 448 line picture) counts the lines of both fields: the picture is half of it.
// `crop` is false for circuit 1 of the merged display: its rectangle sits where it sits to land in the picture, so its DY says nothing
// about lines lost at the bottom (Devil May Cry's message strip).
std::uint32_t display_height(std::uint64_t disp, std::uint64_t smode2, bool crop = true) {
    const std::uint32_t magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    std::uint32_t dh = (static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1;
    if ((smode2 & 3u) == 1u && dh / magv > 576) dh /= 2;
    return shown_lines(dh, crop ? static_cast<std::uint32_t>(disp >> 12) & 0x7FFu : 0u, smode2) / magv;
}
// The two read circuits of the display (PMODE bits EN1 and EN2). With both on, the picture is circuit 2's, and circuit 1 shows over its own
// rectangle of it, blended by PMODE's fixed alpha ALP (MMOD set, bits 8-15). Devil May Cry shows its typed message windows this way: a
// 512x32 strip with the text sits at the end of video memory, circuit 1 puts it at the bottom of the picture, ALP is 0xFF (all of circuit 1),
// and nothing else of the picture shows the text. Positions come from DISPLAY1/DISPLAY2 (DX, DY: where the rectangle starts, in video
// clocks and lines, so the offset between the two is the offset in the picture).
// Only that case is merged: ALP 0xFF, SLBG clear (circuit 2 is the base, not the background colour), the same magnification. Other
// blends stay as they were, circuit 2 alone. In particular God of War and Resident Evil 4 turn both circuits on with ALP 0x80 and
// the same picture one line apart: the hardware's anti-flicker filter, a blur that emulators usually leave out as well.
// Source: the PMODE and DISPLAYx register descriptions of the GS user's manual, and what Devil May Cry shows.
DisplayOverlay display_overlay(std::uint64_t pmode, std::uint64_t smode2, std::uint64_t fb1, std::uint64_t disp1, std::uint64_t disp2) {
    DisplayOverlay o;
    if ((pmode & 3u) != 3u || !(pmode & 0x20u) || (pmode & 0x80u) || ((pmode >> 8) & 0xFFu) != 0xFFu) return o;
    const std::uint32_t magh1 = (static_cast<std::uint32_t>(disp1 >> 23) & 0xFu) + 1, magh2 = (static_cast<std::uint32_t>(disp2 >> 23) & 0xFu) + 1;
    const std::uint32_t magv1 = (static_cast<std::uint32_t>(disp1 >> 27) & 3u) + 1, magv2 = (static_cast<std::uint32_t>(disp2 >> 27) & 3u) + 1;
    if (magh1 != magh2 || magv1 != magv2) return o;
    o.width = ((static_cast<std::uint32_t>(disp1 >> 32) & 0xFFFu) + 1) / magh1;
    o.height = display_height(disp1, smode2, false);
    if (!o.width || !o.height || o.width > 2048 || o.height > 2048) return o;
    o.x = (static_cast<std::int32_t>(disp1 & 0xFFFu) - static_cast<std::int32_t>(disp2 & 0xFFFu)) / static_cast<std::int32_t>(magh2);
    o.y = (static_cast<std::int32_t>((disp1 >> 12) & 0x7FFu) - static_cast<std::int32_t>((disp2 >> 12) & 0x7FFu)) / static_cast<std::int32_t>(magv2);
    o.fbp = (static_cast<std::uint32_t>(fb1) & 0x1FFu) * 32;
    o.fbw = static_cast<std::uint32_t>(fb1 >> 9) & 0x3Fu;
    o.psm = static_cast<std::uint32_t>(fb1 >> 15) & 0x1Fu;
    o.dbx = static_cast<std::uint32_t>(fb1 >> 32) & 0x7FFu;
    o.dby = static_cast<std::uint32_t>(fb1 >> 43) & 0x7FFu;
    o.active = true;
    return o;
}
std::uint64_t g_writes[128];
std::vector<std::uint32_t> g_gpu_frame;
bool g_odd_field = false;

struct Transfer {
    bool active = false;
    std::uint32_t x = 0, y = 0;
    std::uint32_t pending_bits = 0, pending = 0;
} g_transfer;

std::uint32_t g_off32[32][64], g_offz32[32][64];
std::uint32_t g_off16[64][64], g_off16s[64][64], g_offz16[64][64], g_offz16s[64][64];
std::uint32_t g_off8[64][128], g_off4[128][128];

const int kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
const int kBlockZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15}, {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
const int kColumn32[2][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}};
const int kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
const int kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
const int kBlockZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23}, {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
const int kBlockZ16S[8][4] = {{24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3}, {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};
const int kColumn16[2][16] = {{0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27}, {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}};
const int kColumnWord[2][2][8] = {{{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}}, {{8, 9, 12, 13, 0, 1, 4, 5}, {10, 11, 14, 15, 2, 3, 6, 7}}};

void build_tables() {
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            const int column = (y & 7) / 2, cy = y & 1, cx = x & 7;
            g_off32[y][x] = static_cast<std::uint32_t>(kBlock32[y / 8][x / 8] * 256 + column * 64 + kColumn32[cy][cx] * 4);
            g_offz32[y][x] = static_cast<std::uint32_t>(kBlockZ32[y / 8][x / 8] * 256 + column * 64 + kColumn32[cy][cx] * 4);
        }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int column = (y & 7) / 2, cy = y & 1, cx = x & 15;
            const int inner = column * 64 + kColumn16[cy][cx] * 2;
            g_off16[y][x] = static_cast<std::uint32_t>(kBlock16[y / 8][x / 16] * 256 + inner);
            g_off16s[y][x] = static_cast<std::uint32_t>(kBlock16S[y / 8][x / 16] * 256 + inner);
            g_offz16[y][x] = static_cast<std::uint32_t>(kBlockZ16[y / 8][x / 16] * 256 + inner);
            g_offz16s[y][x] = static_cast<std::uint32_t>(kBlockZ16S[y / 8][x / 16] * 256 + inner);
        }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x) {
            const int block = kBlock32[(y / 16) & 3][(x / 16) & 7];
            int wx = x % 16, wy = y % 16;
            const int column = wy / 4;
            wy %= 4;
            std::uint32_t table = static_cast<std::uint32_t>((wy & 2) >> 1);
            const std::uint32_t byte = static_cast<std::uint32_t>(((wx & 8) >> 2) + ((wy & 2) >> 1));
            table ^= static_cast<std::uint32_t>((y / 4) & 1);
            g_off8[y][x] = static_cast<std::uint32_t>(block * 256 + column * 64 + kColumnWord[table][wy & 1][wx & 7] * 4) + byte;
        }
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const int block = kBlock16[(y / 16) & 7][(x / 32) & 3];
            int wx = x % 32, wy = y % 16;
            const int column = wy / 4;
            wy %= 4;
            const std::uint32_t shift = static_cast<std::uint32_t>((wx & 0x18) + ((wy & 2) << 1));
            const std::uint32_t nibble = shift / 4;
            std::uint32_t table = static_cast<std::uint32_t>((wy & 2) >> 1);
            table ^= static_cast<std::uint32_t>(column & 1);
            g_off4[y][x] = static_cast<std::uint32_t>((column * 64 + block * 256 + kColumnWord[table][wy & 1][wx & 7] * 4) * 2) + nibble;
        }
}

std::uint32_t wrap(std::uint32_t a) { return a & (kMemorySize - 1); }

template <typename T>
T get(std::uint32_t a) {
    T v;
    std::memcpy(&v, g_memory + wrap(a), sizeof v);
    return v;
}

template <typename T>
void put(std::uint32_t a, T v) { std::memcpy(g_memory + wrap(a), &v, sizeof v); }

std::uint32_t page_address(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t pw, std::uint32_t ph) {
    const std::uint32_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    return bp * 256 + ((x / pw) + (y / ph) * per_row) * 8192;
}

void raise(std::uint32_t bit) {
    g_csr |= bit;
    if (!(g_imr & (bit << 8))) hw::intc_raise(0);
}

std::uint32_t bits_per_pixel(std::uint32_t psm) {
    switch (psm) {
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    case PSMT8H: return 8;
    case PSMT4HL: case PSMT4HH: return 4;
    default: return 32;
    }
}

struct DownloadKey {
    std::uint64_t buf, pos, reg;
    std::uint32_t offset, qwords;
    bool operator==(const DownloadKey &) const = default;
};

struct CachedDownload {
    DownloadKey key;
    std::vector<std::uint8_t> data;
    std::uint64_t used;
    std::uint64_t frame = 0;  // the frame the game last asked for this rectangle (frames_run)
};

// A cached download stands in for a fresh one only while the game reads the same rectangle every frame or so (the case the cache is
// for: the answer is then one frame old). A read of a rectangle last asked for longer ago than this waits for the real contents:
// Urban Reign reads its pause background once per pause, and the cache gave it the picture of the pause before.
constexpr std::uint64_t kCacheFrames = 2;

std::mutex g_cache_mutex;
std::vector<CachedDownload> g_download_cache;
std::uint64_t g_cache_clock = 0;
DownloadKey g_shadow{};

void remember(const DownloadKey &key, const std::uint8_t *data, std::size_t bytes) {
    std::lock_guard lock(g_cache_mutex);
    auto it = std::find_if(g_download_cache.begin(), g_download_cache.end(), [&](const CachedDownload &c) { return c.key == key; });
    if (it == g_download_cache.end()) {
        if (g_download_cache.size() >= 16)
            g_download_cache.erase(std::min_element(g_download_cache.begin(), g_download_cache.end(),
                                                    [](const CachedDownload &a, const CachedDownload &b) { return a.used < b.used; }));
        it = g_download_cache.insert(g_download_cache.end(), CachedDownload{key, {}, 0});
    }
    it->data.assign(data, data + bytes);
    it->used = ++g_cache_clock;
    it->frame = frames_run();
}

void forget(const DownloadKey &key) {
    std::lock_guard lock(g_cache_mutex);
    std::erase_if(g_download_cache, [&](const CachedDownload &c) { return c.key == key; });
}

#ifdef PS2_GPU
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_seen_ranges;
std::uint32_t g_transfer_serial = 0, g_stale_first = 0, g_stale_bytes = 0;
bool g_transfer_stale = false;

void download_source(const State &s) {
    const std::uint32_t sbp = static_cast<std::uint32_t>(s.bitbltbuf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(s.bitbltbuf >> 16) & 0x3Fu;
    const std::uint32_t spsm = static_cast<std::uint32_t>(s.bitbltbuf >> 24) & 0x3Fu;
    const std::uint32_t x = static_cast<std::uint32_t>(s.trxpos) & 0x7FFu, y = static_cast<std::uint32_t>(s.trxpos >> 16) & 0x7FFu;
    const std::uint32_t w = static_cast<std::uint32_t>(s.trxreg) & 0xFFFu, h = static_cast<std::uint32_t>(s.trxreg >> 32) & 0xFFFu;
    const bool half = bits_per_pixel(spsm) == 16;
    const std::uint32_t pw = spsm == PSMT8 || spsm == PSMT4 ? 128 : 64, ph = spsm == PSMT4 ? 128 : spsm == PSMT8 || half ? 64 : 32;
    const std::uint32_t first = page_address(sbp, sbw, x, y, pw, ph);
    const std::uint32_t end = page_address(sbp, sbw, x + w - 1, y + h - 1, pw, ph) + 8192;
    ++g_transfer_serial;
    g_transfer_stale = false;
    if (!w || !h || x + w > 2048 || y + h > 2048 || end > kMemorySize) {
        gpu::download(g_memory);
        return;
    }
    const std::pair<std::uint32_t, std::uint32_t> range{first, end - first};
    if (queue::active() && std::find(g_seen_ranges.begin(), g_seen_ranges.end(), range) != g_seen_ranges.end()) {
        gpu::download_async(g_memory, first, end - first, g_transfer_serial);
        g_transfer_stale = true;
        g_stale_first = first;
        g_stale_bytes = end - first;
        return;
    }
    gpu::download(g_memory, first, end - first);
    g_seen_ranges.push_back(range);
    if (g_seen_ranges.size() > 8) g_seen_ranges.erase(g_seen_ranges.begin());
}
#endif

void exact_download() {
#ifdef PS2_GPU
    if (!g_transfer_stale) return;
    g_transfer_stale = false;
    if (!gpu::finish_download(g_transfer_serial)) gpu::download(g_memory, g_stale_first, g_stale_bytes);
#endif
}

void transfer_pixel(std::uint32_t value) {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu;
    const std::uint32_t dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    const std::uint32_t dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu;
    const std::uint32_t dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu;
    const std::uint32_t rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!g_transfer.active || !rrw) return;
    write_pixel(dpsm, dbp, dbw, (dsax + g_transfer.x) & 0x7FFu, (dsay + g_transfer.y) & 0x7FFu, value);
    if (++g_transfer.x >= rrw) {
        g_transfer.x = 0;
        if (++g_transfer.y >= rrh) g_transfer.active = false;
    }
}

std::uint8_t g_carry[4];
std::size_t g_carried = 0;

void feed_image(const std::uint8_t *data, std::size_t bytes) {
#ifdef PS2_GPU
    if (gpu_on() && (g_state.trxdir & 3u) == 0) gpu::image_data(data, bytes);
#endif
    if (!cpu_raster()) return;
    if (!g_transfer.active || (g_state.trxdir & 3u) != 0) return;
    const std::uint32_t psm = static_cast<std::uint32_t>(g_state.bitbltbuf >> 56) & 0x3Fu;
    const std::uint32_t bits = bits_per_pixel(psm);
    if (bits == 24) {
        for (std::size_t i = 0; i < bytes && g_transfer.active; ++i) {
            g_carry[g_carried++] = data[i];
            if (g_carried == 3) {
                transfer_pixel(g_carry[0] | (g_carry[1] << 8) | (g_carry[2] << 16));
                g_carried = 0;
            }
        }
        return;
    }
    for (std::size_t bit = 0; bit + bits <= bytes * 8 && g_transfer.active; bit += bits) {
        std::uint32_t v = 0;
        if (bits == 4) v = (data[bit / 8] >> (bit & 4)) & 15u;
        else std::memcpy(&v, data + bit / 8, bits / 8);
        transfer_pixel(v);
    }
}

void local_copy() {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t sbp = static_cast<std::uint32_t>(buf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, spsm = static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
    const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    const std::uint32_t ssax = static_cast<std::uint32_t>(pos) & 0x7FFu, ssay = static_cast<std::uint32_t>(pos >> 16) & 0x7FFu;
    const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t dir = static_cast<std::uint32_t>(pos >> 59) & 3u;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(rrw) * rrh);
    for (std::uint32_t y = 0; y < rrh; ++y)
        for (std::uint32_t x = 0; x < rrw; ++x)
            pixels[y * rrw + x] = read_pixel(spsm, sbp, sbw, (ssax + x) & 0x7FFu, (ssay + y) & 0x7FFu);
    for (std::uint32_t y = 0; y < rrh; ++y) {
        const std::uint32_t sy = (dir & 2u) ? rrh - 1 - y : y;
        for (std::uint32_t x = 0; x < rrw; ++x) {
            const std::uint32_t sx = (dir & 1u) ? rrw - 1 - x : x;
            write_pixel(dpsm, dbp, dbw, (dsax + x) & 0x7FFu, (dsay + y) & 0x7FFu, pixels[sy * rrw + sx]);
        }
    }
}

void load_clut(std::uint64_t tex0) {
    const std::uint32_t cld = static_cast<std::uint32_t>(tex0 >> 61) & 7u;
    static std::uint32_t cbp0 = ~0u, cbp1 = ~0u;
    const std::uint32_t cbp = static_cast<std::uint32_t>(tex0 >> 37) & 0x3FFFu;
    const std::uint32_t before0 = cbp0, before1 = cbp1;
    bool skipped = false;
    switch (cld) {
    case 0: skipped = true; break;
    case 1: break;
    case 2: cbp0 = cbp; break;
    case 3: cbp1 = cbp; break;
    case 4: if (cbp0 == cbp) skipped = true; cbp0 = cbp; break;
    case 5: if (cbp1 == cbp) skipped = true; cbp1 = cbp; break;
    default: skipped = true; break;
    }
    if (trace::traced(trace::g_frame))
        std::fprintf(stderr, "[gs-clut] f%llu after #%llu tex0 %016llX cld %u cbp %X csa %llu csm %llu cpsm %llX psm %llX | cbp0 %X cbp1 %X | %s\n", static_cast<unsigned long long>(trace::g_frame),
                     static_cast<unsigned long long>(trace::g_number), static_cast<unsigned long long>(tex0), cld, cbp, static_cast<unsigned long long>((tex0 >> 56) & 0x1Fu),
                     static_cast<unsigned long long>((tex0 >> 55) & 1u), static_cast<unsigned long long>((tex0 >> 51) & 0xFu), static_cast<unsigned long long>((tex0 >> 20) & 0x3Fu), before0, before1,
                     skipped ? "skipped" : "loaded");
    if (skipped) return;
    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;
    const std::uint32_t cpsm = static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;
    const std::uint32_t csm = static_cast<std::uint32_t>(tex0 >> 55) & 1u;
    const std::uint32_t csa = static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu;
#ifdef PS2_GPU
    if (gpu_on()) gpu::load_clut(tex0, g_state.texclut);
#endif
    if (!cpu_raster()) return;
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const std::uint32_t entries = eight ? 256u : 16u;
    const std::uint32_t texclut = static_cast<std::uint32_t>(g_state.texclut);
    for (std::uint32_t i = 0; i < entries; ++i) {
        std::uint32_t x, y;
        if (csm) {
            const std::uint32_t cbw = texclut & 0x3Fu, cou = (texclut >> 6) & 0x3Fu, cov = (texclut >> 12) & 0x3FFu;
            x = cou * 16 + i;
            y = cov;
            const std::uint32_t c = read_pixel(cpsm == 0 ? PSMCT32 : cpsm, cbp, cbw, x, y);
            const std::uint32_t slot = (csa * 16 + i) & 511u;
            g_state.clut[slot] = c;
            continue;
        }
        if (eight) {
            const std::uint32_t j = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
            x = j & 15u;
            y = j >> 4;
        } else {
            x = i & 7u;
            y = i >> 3;
        }
        const std::uint32_t c = read_pixel(cpsm == 0 ? PSMCT32 : cpsm, cbp, 1, x, y);
        const std::uint32_t slot = eight ? i : ((csa & 15u) * 16u + i) & 255u;
        g_state.clut[slot] = c;
    }
}

}

std::uint8_t *memory() { return g_memory; }

void swizzle_tables(std::vector<std::uint32_t> &out) {
    out.clear();
    for (auto &row : g_off32) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz32) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off16) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off16s) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz16) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz16s) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off8) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off4) out.insert(out.end(), std::begin(row), std::end(row));
}

// The base pointer (in 256-byte blocks) and buffer width of mip levels 0 to 6 (GS User's Manual: TEX0, TEX1 MTBA, MIPTBP1/2; docs/mipmap.md).
// MIPTBP1 holds levels 1 to 3 and MIPTBP2 levels 4 to 6, each as a 14-bit base pointer and a 6-bit width, 20 bits a level.
// With MTBA = 1 the chip works out levels 1 to 3 itself; the rule used here (each level right after the one before, the buffer width halved)
// is the natural packing and a guess in its details: the manual's restrictions on that mode are not checked.
void mip_levels(const Context &c, std::uint32_t bp[7], std::uint32_t bw[7]) {
    bp[0] = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
    bw[0] = static_cast<std::uint32_t>(c.tex0 >> 14) & 0x3Fu;
    for (int n = 1; n <= 6; ++n) {
        const std::uint64_t r = n <= 3 ? c.miptbp1 : c.miptbp2;
        const int shift = ((n - 1) % 3) * 20;
        bp[n] = static_cast<std::uint32_t>(r >> shift) & 0x3FFFu;
        bw[n] = static_cast<std::uint32_t>(r >> (shift + 14)) & 0x3Fu;
    }
    if (!((c.tex1 >> 9) & 1u)) return;
    const std::uint32_t psm = static_cast<std::uint32_t>(c.tex0 >> 20) & 0x3Fu;
    const std::uint32_t tw = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 26) & 0xFu, 10u), th = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 30) & 0xFu, 10u);
    for (std::uint32_t n = 1; n <= 3; ++n) {
        const std::uint32_t w = 1u << (tw > n - 1 ? tw - (n - 1) : 0u), h = 1u << (th > n - 1 ? th - (n - 1) : 0u);
        const std::uint32_t bytes = w * h * bits_per_pixel(psm) / 8u;
        bp[n] = (bp[n - 1] + std::max(1u, (bytes + 255u) / 256u)) & 0x3FFFu;
        bw[n] = std::max(1u, bw[n - 1] / 2u);
    }
}

std::uint32_t address32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    return wrap(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
}

std::uint32_t read_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    switch (psm) {
    case PSMCT32: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
    case PSMCT24: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) & 0xFFFFFFu;
    case PSMZ32: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63]);
    case PSMZ24: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63]) & 0xFFFFFFu;
    case PSMCT16: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16[y & 63][x & 63]);
    case PSMCT16S: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16s[y & 63][x & 63]);
    case PSMZ16: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16[y & 63][x & 63]);
    case PSMZ16S: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16s[y & 63][x & 63]);
    case PSMT8: return get<std::uint8_t>(page_address(bp, bw, x, y, 128, 64) + g_off8[y & 63][x & 127]);
    case PSMT4: {
        const std::uint32_t n = g_off4[y & 127][x & 127];
        const std::uint8_t b = get<std::uint8_t>(page_address(bp, bw, x, y, 128, 128) + n / 2);
        return (n & 1u) ? b >> 4 : b & 15u;
    }
    case PSMT8H: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 24;
    case PSMT4HL: return (get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 24) & 15u;
    case PSMT4HH: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 28;
    default: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
    }
}

void write_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t value) {
    switch (psm) {
    case PSMCT32: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63], value); break;
    case PSMZ32: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63], value); break;
    case PSMCT24: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xFF000000u) | (value & 0xFFFFFFu));
        break;
    }
    case PSMZ24: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xFF000000u) | (value & 0xFFFFFFu));
        break;
    }
    case PSMCT16: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMCT16S: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16s[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMZ16: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMZ16S: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16s[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMT8: put<std::uint8_t>(page_address(bp, bw, x, y, 128, 64) + g_off8[y & 63][x & 127], static_cast<std::uint8_t>(value)); break;
    case PSMT4: {
        const std::uint32_t n = g_off4[y & 127][x & 127];
        const std::uint32_t a = page_address(bp, bw, x, y, 128, 128) + n / 2;
        std::uint8_t b = get<std::uint8_t>(a);
        b = (n & 1u) ? static_cast<std::uint8_t>((b & 0x0Fu) | ((value & 15u) << 4)) : static_cast<std::uint8_t>((b & 0xF0u) | (value & 15u));
        put<std::uint8_t>(a, b);
        break;
    }
    case PSMT8H: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0x00FFFFFFu) | (value << 24));
        break;
    }
    case PSMT4HL: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xF0FFFFFFu) | ((value & 15u) << 24));
        break;
    }
    case PSMT4HH: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0x0FFFFFFFu) | ((value & 15u) << 28));
        break;
    }
    default: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63], value); break;
    }
}

void init() {
    static bool built = false;
    if (built) return;
    build_tables();
    built = true;
#ifdef PS2_GPU
    const char *backend = std::getenv("PS2_GS");
    const gpu::Mode wanted = !backend || std::strcmp(backend, "gpu") == 0 ? gpu::Mode::primary : std::strcmp(backend, "both") == 0 ? gpu::Mode::mirror : gpu::Mode::off;
    if (wanted != gpu::Mode::off && !gpu::init(wanted)) std::fprintf(stderr, "[gs] WebGPU unavailable, using the software renderer\n");
#endif
}

void render_reset() {
#ifdef PS2_GPU
    g_seen_ranges.clear();
    g_transfer_stale = false;
#endif
    init();
    std::memset(&g_state, 0, sizeof g_state);
    std::memset(g_memory, 0, sizeof g_memory);
    ++g_setup_generation;
    g_transfer = Transfer{};
    g_primitives = 0;
    raster::reset();
}

// A reset through CSR clears the drawing environment, not just the vertex queue,
// as PCSX2 does: Downhill Domination's movie player leaves SCANMSK set for it.
void soft_reset() {
    std::uint32_t clut[512];
    std::memcpy(clut, g_state.clut, sizeof clut);
    std::memset(&g_state, 0, sizeof g_state);
    std::memcpy(g_state.clut, clut, sizeof clut);
    ++g_setup_generation;
}

void reset() {
    g_shadow = DownloadKey{};
    {
        std::lock_guard lock(g_cache_mutex);
        g_download_cache.clear();
    }
    g_csr = 0x551B4000ull;
    g_imr = 0x7F00ull;
    g_finish_pending = 0;
    g_finish_queued = 0;
    if (queue::active()) queue::post(queue::kReset, 0, 0, nullptr, 0);
    else render_reset();
}

// Draw trace, to find which draws make a part of the picture (docs/gow2-shadows.md, "The draw trace"):
// PS2_TRACE_DRAWS=<frame>[,<count>] prints the GS draws of <count> frames (default 1) from <frame> on, to stderr. A frame
// is the time from one vertical blank to the next, numbered like the --frames-dir pictures and --press (the numbers go on
// after --load-state); the draws of frame N first show in the picture of frame N+1 or later. Primitives in a row that draw
// with the same registers share one line: their numbers within the frame, how many, the box they cover, the first one's
// vertices, then the registers. Image transfers into GS memory and copies inside it get a [gs-xfer] line.
// PS2_DRAW_RANGE=<first>,<last> draws only the primitives with those numbers in the traced frames and skips the others,
// to see what the frame looks like part way through; PS2_DRAW_SKIP=<first>,<last> skips just those, to see what they add.
// PS2_TRACE_PIXEL=<x>,<y> adds a [gs-pixel] line for every primitive of the traced frames that reaches that pixel (of any
// buffer): what became of it (drawn, or which test stopped it), the pixel and depth before and after (software renderer).
// Exact frame borders need one thread (--frames-dir or PS2_ONE_THREAD=1); with threads they can be off by a few draws.
namespace trace {

const char *spec() {
    static const char *value = std::getenv("PS2_TRACE_DRAWS");
    return value;
}

extern const bool g_any = std::getenv("PS2_TRACE_DRAWS") != nullptr;

std::uint64_t g_frame = 0;  // frame of the draws the render side is doing now
std::uint64_t g_number = 0; // primitives so far in this frame
std::string g_state;        // registers of the line being collected
std::uint64_t g_first = 0, g_count = 0, g_line_frame = 0;
int g_x0, g_y0, g_x1, g_y1;
std::string g_vertices;

bool traced(std::uint64_t frame) {
    if (!spec()) [[likely]] return false;
    static const std::uint64_t from = std::strtoull(spec(), nullptr, 10);
    static const std::uint64_t count = std::strchr(spec(), ',') ? std::strtoull(std::strchr(spec(), ',') + 1, nullptr, 10) : 1;
    return frame >= from && frame < from + count;
}

void flush() {
    if (!g_count) return;
    std::fprintf(stderr, "[gs-draw] f%llu #%llu-%llu n%llu box %d,%d-%d,%d | %s| %s\n", static_cast<unsigned long long>(g_line_frame),
                 static_cast<unsigned long long>(g_first), static_cast<unsigned long long>(g_first + g_count - 1), static_cast<unsigned long long>(g_count), g_x0, g_y0,
                 g_x1, g_y1, g_vertices.c_str(), g_state.c_str());
    g_count = 0;
}

void frame_start(std::uint64_t frame) {
    if (spec()) flush();
    g_frame = frame;
    g_number = 0;
    raster::g_probe.on = false;
}

std::string registers(const State &s, std::uint64_t attr) {
    const Context &c = s.ctx[(attr >> 9) & 1u];
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    char line[1024];
    std::snprintf(line, sizeof line,
                  "prim %llu%s%s%s%s%s%s ctx%llu | FRAME fbp %llX fbw %llu psm %02llX fbmsk %08llX | ZBUF zbp %llX psm %02llX zmsk %llu | TEST ate %llu atst %llu aref %02llX"
                  " afail %llu date %llu datm %llu zte %llu ztst %llu | ALPHA a%llu b%llu c%llu d%llu fix %02llX pabe %llu fba %llu colclamp %llu | TEX0 tbp %llX tbw %llu"
                  " psm %02llX %llux%llu tcc %llu tfx %llu cbp %llX cpsm %02llX csm %llu csa %llu | TEX1 lcm %llu mxl %llu mmag %llu mmin %llu l %llu k %llu | CLAMP wms %llu"
                  " wmt %llu u %llu-%llu v %llu-%llu | TEXA ta0 %02llX aem %llu ta1 %02llX | dthe %llu dimx %016llX | scissor %llu,%llu-%llu,%llu scanmsk %llu ",
                  u(s.prim & 7u), (attr & 0x8u) ? " IIP" : "", (attr & 0x10u) ? " TME" : "", (attr & 0x20u) ? " FGE" : "", (attr & 0x40u) ? " ABE" : "",
                  (attr & 0x80u) ? " AA1" : "", (attr & 0x100u) ? " FST" : "", u((attr >> 9) & 1u), u(c.frame & 0x1FFu) * 32, u((c.frame >> 16) & 0x3Fu),
                  u((c.frame >> 24) & 0x3Fu), u(c.frame >> 32), u(c.zbuf & 0x1FFu) * 32, u(0x30u | ((c.zbuf >> 24) & 0xFu)), u((c.zbuf >> 32) & 1u), u(c.test & 1u),
                  u((c.test >> 1) & 7u), u((c.test >> 4) & 0xFFu), u((c.test >> 12) & 3u), u((c.test >> 14) & 1u), u((c.test >> 15) & 1u), u((c.test >> 16) & 1u),
                  u((c.test >> 17) & 3u), u(c.alpha & 3u), u((c.alpha >> 2) & 3u), u((c.alpha >> 4) & 3u), u((c.alpha >> 6) & 3u), u((c.alpha >> 32) & 0xFFu),
                  u(s.pabe & 1u), u(c.fba & 1u), u(s.colclamp & 1u), u(c.tex0 & 0x3FFFu), u((c.tex0 >> 14) & 0x3Fu), u((c.tex0 >> 20) & 0x3Fu),
                  1ull << ((c.tex0 >> 26) & 0xFu), 1ull << ((c.tex0 >> 30) & 0xFu), u((c.tex0 >> 34) & 1u), u((c.tex0 >> 35) & 3u), u((c.tex0 >> 37) & 0x3FFFu),
                  u((c.tex0 >> 51) & 0xFu), u((c.tex0 >> 55) & 1u), u((c.tex0 >> 56) & 0x1Fu), u(c.tex1 & 1u), u((c.tex1 >> 2) & 7u), u((c.tex1 >> 5) & 1u),
                  u((c.tex1 >> 6) & 7u), u((c.tex1 >> 19) & 3u), u((c.tex1 >> 32) & 0xFFFu), u(c.clamp & 3u), u((c.clamp >> 2) & 3u), u((c.clamp >> 4) & 0x3FFu),
                  u((c.clamp >> 14) & 0x3FFu), u((c.clamp >> 24) & 0x3FFu), u((c.clamp >> 34) & 0x3FFu), u(s.texa & 0xFFu), u((s.texa >> 15) & 1u),
                  u((s.texa >> 32) & 0xFFu), u(s.dthe & 1u), u(s.dimx), u(c.scissor & 0x7FFu), u((c.scissor >> 32) & 0x7FFu), u((c.scissor >> 16) & 0x7FFu),
                  u((c.scissor >> 48) & 0x7FFu), u(s.scanmsk & 3u));
    return line;
}

// PS2_DUMP_TEXAT=<n>,<path prefix>: at primitive <n> of the traced frame, writes the texture it uses (raw indices or colours,
// <prefix>_tex.pgm / .ppm) and the 256 palette entries the software renderer holds (<prefix>_clut.txt).
void dump_texture(const State &s, std::uint64_t attr, const char *spec) {
    const Context &c = s.ctx[(attr >> 9) & 1u];
    const char *comma = std::strchr(spec, ',');
    if (!comma) return;
    const std::string prefix = comma + 1;
    const std::uint32_t tbp = static_cast<std::uint32_t>(c.tex0 & 0x3FFFu), tbw = static_cast<std::uint32_t>((c.tex0 >> 14) & 0x3Fu), psm = static_cast<std::uint32_t>((c.tex0 >> 20) & 0x3Fu);
    const std::uint32_t w = 1u << ((c.tex0 >> 26) & 0xFu), h = 1u << ((c.tex0 >> 30) & 0xFu);
    const bool indexed = psm == PSMT8 || psm == PSMT4;
    std::FILE *f = os::open((prefix + (indexed ? "_tex.pgm" : "_tex.ppm")).c_str(), "wb");
    if (f) {
        std::fprintf(f, indexed ? "P5\n%u %u\n255\n" : "P6\n%u %u\n255\n", w, h);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint32_t v = read_pixel(psm, tbp, tbw, x, y);
                const std::uint8_t px[3] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16)};
                std::fwrite(px, 1, indexed ? 1 : 3, f);
            }
        std::fclose(f);
    }
    f = os::open((prefix + "_clut.txt").c_str(), "wb");
    if (f) {
        for (int i = 0; i < 256; ++i) std::fprintf(f, "%02X %08X\n", i, s.clut[i]);
        std::fclose(f);
    }
}

// Called for every primitive; false: skip drawing it (PS2_DRAW_RANGE).
bool draw(const State &s, int needed) {
    if (!traced(g_frame)) [[likely]] return true;
    static const char *range = std::getenv("PS2_DRAW_RANGE");
    static const std::uint64_t first = range ? std::strtoull(range, nullptr, 10) : 0;
    static const std::uint64_t last = range && std::strchr(range, ',') ? std::strtoull(std::strchr(range, ',') + 1, nullptr, 10) : ~0ull;
    static const char *skip = std::getenv("PS2_DRAW_SKIP");
    static const std::uint64_t skip_first = skip ? std::strtoull(skip, nullptr, 10) : 1;
    static const std::uint64_t skip_last = skip && std::strchr(skip, ',') ? std::strtoull(std::strchr(skip, ',') + 1, nullptr, 10) : skip_first;
    const std::uint64_t n = g_number++;
    if (static const char *vd = std::getenv("PS2_DUMP_VU1AT"); vd && std::strchr(vd, ',') && n == std::strtoull(vd, nullptr, 10)) ps2::vu::trace_dump(std::strchr(vd, ',') + 1);
    static const char *dump_at = std::getenv("PS2_DUMP_TEXAT");
    if (dump_at && n == std::strtoull(dump_at, nullptr, 10)) dump_texture(s, s.prmodecont & 1u ? s.prim : (s.prim & 7u) | (s.prmode & ~7ull), dump_at);
    raster::g_probe = raster::Probe{std::getenv("PS2_TRACE_PIXEL") != nullptr, g_frame, n};
    std::uint64_t attr = s.prim;
    if (!(s.prmodecont & 1u)) attr = (s.prim & 7u) | (s.prmode & ~7ull);
    const Context &c = s.ctx[(attr >> 9) & 1u];
    const int ofx = static_cast<int>(c.xyoffset & 0xFFFFu), ofy = static_cast<int>((c.xyoffset >> 32) & 0xFFFFu);
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < needed; ++i) {
        const int x = (static_cast<int>(s.queue[i].x) - ofx) >> 4, y = (static_cast<int>(s.queue[i].y) - ofy) >> 4;
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    if (static const char *tv = std::getenv("PS2_TRACE_VERTS"); tv && n >= std::strtoull(tv, nullptr, 10) && n <= (std::strchr(tv, ',') ? std::strtoull(std::strchr(tv, ',') + 1, nullptr, 10) : ~0ull)) {
        std::fprintf(stderr, "[gs-vert] f%llu #%llu", static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(n));
        for (int i = 0; i < needed; ++i) {
            const Vertex &v = s.queue[i];
            std::fprintf(stderr, " | %.3f %.3f %u %.5f %.5f %.5f", (static_cast<int>(v.x) - ofx) / 16.0, (static_cast<int>(v.y) - ofy) / 16.0, v.z, v.s, v.t, v.q);
        }
        std::fprintf(stderr, "\n");
    }
    std::string state = registers(s, attr);
    { char tail[32]; std::snprintf(tail, sizeof tail, "| vu1pc %X ", ps2::vu::g_trace_vu1_pc); state += tail; }
    if (g_count && state == g_state && n == g_first + g_count) {
        ++g_count;
        g_x0 = std::min(g_x0, x0), g_y0 = std::min(g_y0, y0), g_x1 = std::max(g_x1, x1), g_y1 = std::max(g_y1, y1);
    } else {
        flush();
        g_state.swap(state);
        g_first = n, g_count = 1, g_line_frame = g_frame;
        g_x0 = x0, g_y0 = y0, g_x1 = x1, g_y1 = y1;
        g_vertices.clear();
        for (int i = 0; i < needed; ++i) {
            const Vertex &v = s.queue[i];
            char text[192];
            std::snprintf(text, sizeof text, "v%d %.4g,%.4g z %X rgba %02X%02X%02X%02X stq %.4g,%.4g,%.4g uv %.4g,%.4g ", i, (static_cast<int>(v.x) - ofx) / 16.0,
                          (static_cast<int>(v.y) - ofy) / 16.0, v.z, v.r, v.g, v.b, v.a, v.s, v.t, v.q, v.u / 16.0, v.v / 16.0);
            g_vertices += text;
        }
    }
    return n >= first && n <= last && !(skip && n >= skip_first && n <= skip_last);
}

void transfer(const State &s, std::uint64_t dir) {
    if (!traced(g_frame)) [[likely]] return;
    flush();
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    const std::uint64_t b = s.bitbltbuf;
    std::fprintf(stderr, "[gs-xfer] f%llu after #%llu dir %llu | src bp %llX bw %llu psm %02llX at %llu,%llu | dst bp %llX bw %llu psm %02llX at %llu,%llu | %llux%llu\n",
                 u(g_frame), u(g_number), u(dir & 3u), u(b & 0x3FFFu), u((b >> 16) & 0x3Fu), u((b >> 24) & 0x3Fu), u(s.trxpos & 0x7FFu), u((s.trxpos >> 16) & 0x7FFu),
                 u((b >> 32) & 0x3FFFu), u((b >> 48) & 0x3Fu), u((b >> 56) & 0x3Fu), u((s.trxpos >> 32) & 0x7FFu), u((s.trxpos >> 48) & 0x7FFu), u(s.trxreg & 0xFFFu),
                 u((s.trxreg >> 32) & 0xFFFu));
}

}

// local change (PS2_UI_SQUEEZE, for a 4:3 game shown 16:9): the game lays its menus and HUD out for a 4:3 screen, so on a 16:9
// picture they come out a third too wide. A draw counts as interface when it goes to the 640 wide picture and either ignores depth
// (test off or ALWAYS) or sits at the very front of a 16-bit depth buffer; the 3D scene tests depth (GEQUAL). Such a draw is narrowed
// (kick() below). Draws that cover the whole screen (fades, backdrops, films) stay as they are.
//   PS2_UI_SQUEEZE=1    narrow to 75 %, the true 4:3 shape
//   PS2_UI_SQUEEZE=NN   narrow to NN % (50 to 99): 85 is a little wider than true
// What a shape is narrowed towards depends on the group of interface shapes it belongs to, so that a HUD in a corner stays in its
// corner: the screen is a grid of 5 x 5 pixel cells, every interface shape marks the cells its box covers, and once per refresh
// the cells marked in the last few refreshes are joined into groups of touching cells. Large shapes mark a box 8 pixels smaller
// on every side, so that two panels that merely touch (a dialog next to a logo) stay two groups; small shapes (letters, icons)
// mark their whole box and so join the panel they lie on. A group is narrowed towards the left edge when it reaches that edge,
// ends before the middle of the screen and is a solid block (at least 40 pixels high and 3000 square pixels: a panel, not a
// line of text); the same for the right edge. All other groups are narrowed towards the middle. A shape takes the group of the
// cell in the middle of its box, as found one refresh earlier.
namespace ui_squeeze {
constexpr int kCell = 5, kCols = 128, kRows = 106;
std::uint64_t g_stamp[kRows][kCols];
std::uint8_t g_anchor[kRows][kCols];  // 0 middle, 1 left edge, 2 right edge
std::uint64_t g_frame = 0;

int percent() {
    static const int value = [] {
        const char *e = std::getenv("PS2_UI_SQUEEZE");
        const int v = e ? std::atoi(e) : 0;
        return v == 1 ? 75 : (v >= 50 && v <= 99) ? v : 100;
    }();
    return value;
}

// local change: how each group of interface shapes is narrowed, decided once per refresh.
//  - A solid group that reaches one side edge of the screen and not the other goes to that edge (a HUD in a corner; a queue of
//    icons that grows out of the opposite corner, however far it reaches).
//  - A group that reaches both edges is one of two things. When nearly all of it was there a refresh ago, it is pieces that
//    have grown together (the HUD on the left and the action queue from the right touch while a Sim is busy): every cell keeps
//    what it had, and new cells take what the nearest old cell has, so nothing moves when they touch or part. Otherwise it is
//    something new across the whole screen (a catalogue bar) and goes to the middle in one piece.
//  - A small group (a portrait, an icon, a number next to the HUD) follows the nearest solid group within 80 pixels.
//  - Everything else goes to the middle (dialogs, menus, lines of text).
std::uint8_t g_was_live[kRows][kCols];
std::uint8_t g_was_solid[kRows][kCols];  // the cell was in a solid group at the last refresh with interface shapes
// The last three refreshes in which the game drew interface shapes. A cell counts as on the screen while it was drawn within six
// refreshes, or within those three: at 50 refreshes a second and the game's usual 25 pictures that is the same thing, but at a
// higher frame rate, or in a busy house where the game takes longer over a picture, six refreshes pass between two pictures of the
// game; the HUD's cells then went stale between them, and it was drawn towards the middle like a thing never seen before.
std::uint64_t g_active[3] = {};

std::atomic<bool> g_panel_up{false};  // local change (the PC mouse): a full-screen panel was up at the last regroup

void regroup(std::uint64_t frame) {
    g_panel_up.store(false, std::memory_order_relaxed);
    static std::uint8_t seen[kRows][kCols];
    static std::uint8_t before[kRows][kCols];
    static std::vector<int> cells, queue;
    struct Group {
        int first, count, lo, hi, top, bottom;
        bool solid;
        std::uint8_t anchor;
        bool solid_like = false;  // a small group that went to a screen edge by itself
    };
    static std::vector<Group> groups;
    static const bool log = std::getenv("PS2_UI_SQUEEZE_LOG") != nullptr;
    std::memset(seen, 0, sizeof seen);
    std::memcpy(before, g_anchor, sizeof before);
    std::memset(g_anchor, 0, sizeof g_anchor);
    cells.clear();
    groups.clear();
    const auto live = [&](int r, int c) { return g_stamp[r][c] && (g_stamp[r][c] + 6 >= frame || g_stamp[r][c] >= g_active[2]); };
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kCols; ++c) {
            if (seen[r][c] || !live(r, c)) continue;
            Group g{static_cast<int>(cells.size()), 0, c, c, r, r, false, 0};
            cells.push_back(r * kCols + c);
            seen[r][c] = 1;
            for (std::size_t at = g.first; at < cells.size(); ++at) {
                const int cr = cells[at] / kCols, cc = cells[at] % kCols;
                g.lo = std::min(g.lo, cc); g.hi = std::max(g.hi, cc); g.top = std::min(g.top, cr); g.bottom = std::max(g.bottom, cr);
                for (int dr = -1; dr <= 1; ++dr)
                    for (int dc = -1; dc <= 1; ++dc) {
                        const int nr = cr + dr, nc = cc + dc;
                        if (nr < 0 || nr >= kRows || nc < 0 || nc >= kCols || seen[nr][nc] || !live(nr, nc)) continue;
                        seen[nr][nc] = 1;
                        cells.push_back(nr * kCols + nc);
                    }
            }
            g.count = static_cast<int>(cells.size()) - g.first;
            g.solid = (g.bottom - g.top + 1) * kCell >= 40 && g.count * kCell * kCell >= 3000;
            // Once solid, a group stays solid while most of it is the same cells. The slider of the body editor is a thin wedge
            // that grows and shrinks as it is moved, around the size above: solid it went to the middle, small it followed the
            // panel 70 pixels to its left to the left edge, so it jumped while being dragged.
            // Only for groups at least half a panel's area: a panel that slides in is one solid group, and when it stops, the
            // rounded ends of its header and bar (20x20 and 20x40 pixels in create-a-Sim's colour page) come apart from it; kept
            // solid they went to the middle on their own, half a screen away from their panel at the left edge.
            if (!g.solid && g.count * kCell * kCell >= 1500) {
                int old = 0;
                for (int i = 0; i < g.count; ++i) old += g_was_solid[cells[g.first + i] / kCols][cells[g.first + i] % kCols];
                g.solid = old * 5 >= g.count * 4;
            }
            groups.push_back(g);
        }
    // A panel that fills most of the screen (the buy catalogue, the relationships page) is a screen laid out as a whole:
    // its side rails, its "Help" label and everything else belong where the game put them around it. While one is up,
    // everything is narrowed towards the middle and nothing goes to the edges; rails at the screen's sides would otherwise
    // leave the panel, or the catalogue's halves go to opposite edges. Edge anchoring is for the HUD over the game.
    for (const Group &g : groups) {
        // (448 wide: the full-screen panels are 530-590 pixels wide; dialogs and the main menu's wheel are ~370 and must not
        // send the "Die Sims 2" logo and the HUD towards the middle when they open)
        if (!g.solid || (g.hi - g.lo + 1) * kCell < 448 || (g.bottom - g.top + 1) * kCell < 224) continue;
        if (log) {
            static std::uint64_t said = 0;
            if (frame > said + 60 || frame % 120 == 0) std::fprintf(stderr, "[ui] frame %llu: a panel fills the screen (x %d-%d y %d-%d): everything to the middle\n", static_cast<unsigned long long>(frame), g.lo * kCell, (g.hi + 1) * kCell, g.top * kCell, (g.bottom + 1) * kCell);
            said = frame;
        }
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < kCols; ++c) g_was_live[r][c] = g_was_solid[r][c] = 0;  // (afterwards the HUD is placed afresh)
        g_panel_up.store(true, std::memory_order_relaxed);
        return;
    }
    // "At an edge" is not enough for a wide group: the pause menu's bar runs from 55 to 565 of the 640 pixels and is in the
    // middle, though its glow comes within 70 pixels of the left edge on some frames. A group goes to an edge only when it is
    // clearly nearer to it: the gap on the other side is more than twice the gap on this side plus 100 pixels.
    const auto gap_left = [](const Group &g) { return g.lo * kCell; };
    const auto gap_right = [](const Group &g) { return 640 - (g.hi + 1) * kCell; };
    const auto both = [&](const Group &g) { return gap_left(g) < 70 && gap_right(g) < 70; };
    const auto left = [&](const Group &g) { return gap_left(g) < 70 && (both(g) || gap_right(g) > 2 * gap_left(g) + 100); };
    const auto right = [&](const Group &g) { return gap_right(g) < 70 && (both(g) || gap_left(g) > 2 * gap_right(g) + 100); };
    // solid groups first
    for (Group &g : groups) {
        if (!g.solid) continue;
        if (left(g) != right(g)) {
            g.anchor = left(g) ? 1 : 2;
            for (int i = 0; i < g.count; ++i) g_anchor[cells[g.first + i] / kCols][cells[g.first + i] % kCols] = g.anchor;
            continue;
        }
        if (!left(g)) continue;  // reaches neither edge: the middle
        // reaches both edges
        int old = 0;
        for (int i = 0; i < g.count; ++i) old += g_was_live[cells[g.first + i] / kCols][cells[g.first + i] % kCols];
        g.anchor = 3;  // mixed (for the small groups below: they look at the cells)
        if (old * 5 < g.count * 4) {
            g.anchor = 0;
            if (log) std::fprintf(stderr, "[ui] frame %llu: new group across the screen, y %d-%d, %d cells (%d old): the middle\n", static_cast<unsigned long long>(frame), g.top * kCell, (g.bottom + 1) * kCell, g.count, old);
            continue;
        }
        // old cells keep theirs; new cells take the nearest old cell's (a walk outwards from the old cells, inside the group)
        queue.clear();
        std::memset(seen, 0, sizeof seen);  // (free again: the groups are found)
        for (int i = 0; i < g.count; ++i) {
            const int cell = cells[g.first + i], cr = cell / kCols, cc = cell % kCols;
            if (!g_was_live[cr][cc]) continue;
            g_anchor[cr][cc] = before[cr][cc];
            seen[cr][cc] = 1;
            queue.push_back(cell);
        }
        for (int i = 0; i < g.count; ++i) seen[cells[g.first + i] / kCols][cells[g.first + i] % kCols] |= 2;  // in this group
        for (std::size_t at = 0; at < queue.size(); ++at) {
            const int cr = queue[at] / kCols, cc = queue[at] % kCols;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc) {
                    const int nr = cr + dr, nc = cc + dc;
                    if (nr < 0 || nr >= kRows || nc < 0 || nc >= kCols || seen[nr][nc] != 2) continue;
                    seen[nr][nc] = 3;
                    g_anchor[nr][nc] = g_anchor[cr][cc];
                    queue.push_back(nr * kCols + nc);
                }
        }
    }
    // small groups follow the nearest solid group within 80 pixels (measured between their boxes); one at a screen edge that
    // has no such neighbour goes to that edge when it is more than a speck
    for (Group &g : groups) {
        if (g.solid) continue;
        int best = 80 / kCell + 1;
        const Group *near = nullptr;
        for (const Group &o : groups) {
            if (!o.solid) continue;
            const int dx = std::max(0, std::max(o.lo - g.hi, g.lo - o.hi)), dy = std::max(0, std::max(o.top - g.bottom, g.top - o.bottom));
            const int d = std::max(dx, dy);
            if (d < best) best = d, near = &o;
        }
        std::uint8_t anchor = 0;
        if (near && near->anchor == 3) {
            // a mixed group: what its cell nearest to this group's middle has
            const int mr = (g.top + g.bottom) / 2, mc = (g.lo + g.hi) / 2;
            int closest = 1 << 30;
            for (int i = 0; i < near->count; ++i) {
                const int cell = cells[near->first + i], d = std::abs(cell / kCols - mr) + std::abs(cell % kCols - mc);
                if (d < closest) closest = d, anchor = g_anchor[cell / kCols][cell % kCols];
            }
        } else if (near) {
            anchor = near->anchor;
        } else if (g.count >= 20 && left(g) != right(g)) {
            anchor = left(g) ? 1 : 2;
            g.solid_like = true;
        }
        g.anchor = anchor;
        if (anchor) for (int i = 0; i < g.count; ++i) g_anchor[cells[g.first + i] / kCols][cells[g.first + i] % kCols] = anchor;
    }
    // and a small group with no solid neighbour follows a small one that went to an edge by itself, within 80 pixels: the word
    // "Help" beside its button in the corner of the screen
    for (Group &g : groups) {
        if (g.solid || g.solid_like || g.anchor) continue;
        int best = 80 / kCell + 1;
        const Group *near = nullptr;
        for (const Group &o : groups) {
            if (!o.solid && !o.solid_like) continue;
            const int dx = std::max(0, std::max(o.lo - g.hi, g.lo - o.hi)), dy = std::max(0, std::max(o.top - g.bottom, g.top - o.bottom));
            const int d = std::max(dx, dy);
            if (d < best) best = d, near = &o;
        }
        if (!near || near->solid || !near->anchor) continue;  // (a solid neighbour was looked at above)
        g.anchor = near->anchor;
        for (int i = 0; i < g.count; ++i) g_anchor[cells[g.first + i] / kCols][cells[g.first + i] % kCols] = g.anchor;
    }
    if (log) {
        static std::string last;
        std::string now;
        char text[96];
        for (const Group &g : groups) {
            if (!g.solid) continue;
            std::snprintf(text, sizeof text, " [x %d-%d y %d-%d: %d]", g.lo * kCell, (g.hi + 1) * kCell, g.top * kCell, (g.bottom + 1) * kCell, g.anchor);
            now += text;
        }
        if (now != last) std::fprintf(stderr, "[ui] frame %llu solid groups:%s\n", static_cast<unsigned long long>(frame), now.c_str());
        last = now;
    }
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kCols; ++c) g_was_live[r][c] = live(r, c);
    std::memset(g_was_solid, 0, sizeof g_was_solid);
    for (const Group &g : groups)
        if (g.solid)
            for (int i = 0; i < g.count; ++i) g_was_solid[cells[g.first + i] / kCols][cells[g.first + i] % kCols] = 1;
}

// local change: the anchor that every anchored cell under a box (pixels) has, 0 when there is none or they differ
int area_anchor(int x0, int y0, int x1, int y1) {
    const int c0 = std::clamp(x0 / kCell, 0, kCols - 1), c1 = std::clamp(x1 / kCell, 0, kCols - 1);
    const int r0 = std::clamp(y0 / kCell, 0, kRows - 1), r1 = std::clamp(y1 / kCell, 0, kRows - 1);
    int found = 0;
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c) {
            const int a = g_anchor[r][c];
            if (!a) continue;
            if (found && a != found) return 0;
            found = a;
        }
    return found;
}

// Marks the box (pixels) of an interface shape and returns what to narrow it towards.
int note(int x0, int y0, int x1, int y1, bool visible) {
    const std::uint64_t frame = ps2::frames_run() + 1;
    if (frame != g_frame) {
        regroup(frame);
        g_active[2] = g_active[1], g_active[1] = g_active[0], g_active[0] = frame;
        g_frame = frame;
    }
    const int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    int bx0 = x0, bx1 = x1, by0 = y0, by1 = y1;
    if (x1 - x0 >= 24 && y1 - y0 >= 24) {
        bx0 += 8; bx1 -= 8; by0 += 8; by1 -= 8;
    }
    // A shape that cannot be seen (all its corners fully transparent) is no piece of anything: it marks nothing, or it would
    // join what it covers into one group. (Large shapes do mark: the buy catalogue's backing panel is what makes the whole
    // catalogue one group, and without it the catalogue's left and right parts went to opposite edges of the screen.)
    if (visible && bx1 >= 0 && bx0 < kCols * kCell && by1 >= 0 && by0 < kRows * kCell) {
        const int c0 = std::clamp(bx0 / kCell, 0, kCols - 1), c1 = std::clamp(bx1 / kCell, 0, kCols - 1);
        const int r0 = std::clamp(by0 / kCell, 0, kRows - 1), r1 = std::clamp(by1 / kCell, 0, kRows - 1);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) g_stamp[r][c] = frame;
    }
    const int anchor = g_anchor[std::clamp(my / kCell, 0, kRows - 1)][std::clamp(mx / kCell, 0, kCols - 1)];
    // PS2_UI_SQUEEZE_TRACE=x0,y0,x1,y1: every interface shape whose box meets that box (console pixels), with what it was given
    static int trace[4] = {-1, -1, -1, -1};
    static const bool tracing = [] {
        const char *e = std::getenv("PS2_UI_SQUEEZE_TRACE");
        return e && std::sscanf(e, "%d,%d,%d,%d", &trace[0], &trace[1], &trace[2], &trace[3]) == 4;
    }();
    if (tracing && x1 >= trace[0] && x0 <= trace[2] && y1 >= trace[1] && y0 <= trace[3])
        std::fprintf(stderr, "[ui] frame %llu shape x %d-%d y %d-%d %s -> anchor %d\n", static_cast<unsigned long long>(frame), x0, x1, y0, y1, visible ? "visible" : "invisible", anchor);
    return anchor;
}
}

// local change: PS2_TEX_DUMP=<folder> writes every texture the game draws with into that folder, once each, as a 32-bit TGA
// named <hash>_<width>x<height>.tga. The hash is of the texture's colours as the game would read them (its palette applied), so
// one picture stored with two palettes gives two files, and the same picture loaded again later gives none. This is the first
// half of texture replacement: the files are what an upscaler, or a person, makes better versions from.
// The alpha in the files is the console's (0x80 is opaque), unchanged.
// A texture is looked at again only when image data has been sent to the GS since it was last hashed.
namespace texdump {
std::uint64_t g_uploads = 1;  // counts image transfers to the GS

const std::string &folder() {
    static const std::string dir = [] {
        const char *e = std::getenv("PS2_TEX_DUMP");
        return std::string(e ? e : "");
    }();
    return dir;
}

struct Seen {
    std::uint64_t uploads = 0;
};
std::unordered_map<std::uint64_t, Seen> g_seen;   // by the texture's place and format (TEX0 without the function bits)
std::unordered_set<std::uint64_t> g_written;      // by the hash of its colours

// The texture of TEX0 `tex0` as RGBA, the way the CPU renderer reads a texel (raster.cpp, texel)
void decode(const State &s, std::uint64_t tex0, std::uint32_t w, std::uint32_t h, std::vector<std::uint32_t> &out) {
    const std::uint32_t tbp = static_cast<std::uint32_t>(tex0 & 0x3FFFu), tbw = static_cast<std::uint32_t>((tex0 >> 14) & 0x3Fu), psm = static_cast<std::uint32_t>((tex0 >> 20) & 0x3Fu);
    const std::uint32_t cpsm = static_cast<std::uint32_t>((tex0 >> 51) & 0xFu), csa = static_cast<std::uint32_t>((tex0 >> 56) & 0x1Fu);
    const std::uint32_t ta0 = static_cast<std::uint32_t>(s.texa & 0xFFu), ta1 = static_cast<std::uint32_t>((s.texa >> 32) & 0xFFu);
    const bool aem = (s.texa >> 15) & 1u;
    const auto expand16 = [&](std::uint32_t v) {
        const std::uint32_t r = (v & 31u) << 3, g = ((v >> 5) & 31u) << 3, b = ((v >> 10) & 31u) << 3;
        const std::uint32_t a = (v & 0x8000u) ? ta1 : (aem && !(v & 0x7FFFu)) ? 0u : ta0;
        return r | g << 8 | b << 16 | a << 24;
    };
    out.resize(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t raw = read_pixel(psm, tbp, tbw, x, y);
            std::uint32_t v = raw;
            switch (psm) {
            case PSMCT32: break;
            case PSMCT24: v = (raw & 0xFFFFFFu) | (((aem && !(raw & 0xFFFFFFu)) ? 0u : ta0) << 24); break;
            case PSMCT16: case PSMCT16S: v = expand16(raw); break;
            case PSMT8: case PSMT8H: {
                const std::uint32_t e = s.clut[raw & 255u];
                v = cpsm == PSMCT32 || cpsm == PSMCT24 ? e : expand16(e);
                break;
            }
            case PSMT4: case PSMT4HL: case PSMT4HH: {
                const std::uint32_t e = s.clut[((csa & 15u) * 16u + (raw & 15u)) & 511u];
                v = cpsm == PSMCT32 || cpsm == PSMCT24 ? e : expand16(e);
                break;
            }
            default: break;
            }
            out[static_cast<std::size_t>(y) * w + x] = v;
        }
}

std::uint64_t hash(const std::vector<std::uint32_t> &pixels, std::uint32_t w, std::uint32_t h) {
    std::uint64_t value = 1469598103934665603ull ^ (static_cast<std::uint64_t>(w) << 32 | h);
    for (const std::uint32_t p : pixels) {
        value ^= p;
        value *= 1099511628211ull;
    }
    return value;
}

void see(const State &s, const Context &c) {
    const std::uint64_t key = c.tex0 & ~(7ull << 34) & ~(7ull << 61);  // without TCC, TFX and CLD
    Seen &seen = g_seen[key];
    if (seen.uploads == g_uploads) return;
    seen.uploads = g_uploads;
    const std::uint32_t w = 1u << ((c.tex0 >> 26) & 0xFu), h = 1u << ((c.tex0 >> 30) & 0xFu);
    if (w < 8 || h < 8 || w > 1024 || h > 1024) return;
    static std::vector<std::uint32_t> pixels;
    decode(s, c.tex0, w, h, pixels);
    const std::uint64_t id = hash(pixels, w, h);
    if (!g_written.insert(id).second) return;
    bool plain = true;  // one colour all over: memory that holds no picture (yet)
    for (const std::uint32_t p : pixels) plain = plain && p == pixels[0];
    if (plain) return;
    char name[64];
    std::snprintf(name, sizeof name, "/%016llx_%ux%u.tga", static_cast<unsigned long long>(id), w, h);
    std::FILE *f = os::open(folder() + name, "wb");
    if (!f) return;
    const std::uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8),
                                     static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(h >> 8), 32, 0x28};  // 32 bits, top row first
    std::fwrite(header, 1, sizeof header, f);
    for (const std::uint32_t p : pixels) {
        const std::uint8_t bgra[4] = {static_cast<std::uint8_t>(p >> 16), static_cast<std::uint8_t>(p >> 8), static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(p >> 24)};
        std::fwrite(bgra, 1, 4, f);
    }
    std::fclose(f);
}
}

// Inlined into the PACKED run (write_packed_run): as a call of its own it cost the ship about 0.15 ms of the drawing thread a frame.
[[gnu::always_inline]] static inline void kick(bool draw) {
    State &s = g_state;
    const std::uint32_t prim = static_cast<std::uint32_t>(s.prim) & 7u;
    const int needed = prim == 0 ? 1 : (prim == 1 || prim == 2 || prim == 6) ? 2 : prim == 7 ? 1 : 3;
    if (s.count < needed) return;
    if (draw) {
        if (!trace::g_any || trace::draw(s, needed)) {
            // local change (PS2_UI_SQUEEZE, for a 4:3 game shown 16:9): see ui_squeeze above
            const Vertex *out = s.queue;
            Vertex narrowed[3];
            std::uint64_t old_scissor = 0;
            Context *changed = nullptr;
            if (!texdump::folder().empty()) {
                const std::uint64_t mode = s.prmodecont & 1u ? s.prim : (s.prim & 7u) | (s.prmode & ~7ull);
                if (mode & 0x10u) texdump::see(s, s.ctx[(mode >> 9) & 1u]);  // TME: a textured shape
            }
            bool too_small = false;
            if (s.queue[0].z >= 0xFEF8u) {  // (first, as nearly all of the 3D scene fails it)
                const std::uint64_t mode = s.prmodecont & 1u ? s.prim : (s.prim & 7u) | (s.prmode & ~7ull);
                Context &c = s.ctx[(mode >> 9) & 1u];
                const bool z16 = ((c.zbuf >> 24) & 0xFu) == 0xAu || ((c.zbuf >> 24) & 0xFu) == 0x2u;
                bool front = z16;
                for (int i = 0; front && i < needed; ++i) front = s.queue[i].z >= 0xFEF8u;
                if (((c.frame >> 16) & 0x3Fu) == 10u && front) {  // depth test ALWAYS alone is not enough: the sky backdrop uses it too, at z 1
                    const int ofx = static_cast<int>(c.xyoffset & 0xFFFFu), ofy = static_cast<int>((c.xyoffset >> 32) & 0xFFFFu);
                    int x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
                    for (int i = 0; i < needed; ++i) {
                        const int x = static_cast<int>(s.queue[i].x) - ofx, y = static_cast<int>(s.queue[i].y) - ofy;
                        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
                    }
                    // An interface piece whose box holds no console pixel (the console draws a pixel when its whole-numbered
                    // position is inside the shape) is invisible on the console: the game hides things by shrinking them to
                    // a fraction of a pixel (a 0.56 pixel square in a dialog's corner, thin slivers beside menu entries).
                    // Upscaled, the finer samples found them and they showed as specks and thin lines. They are left out.
                    const auto holds_pixel = [](int a, int b) { return ((a + 15) >> 4) < ((b + 15) >> 4); };  // a whole number in [a, b), 1/16 pixels
                    too_small = !holds_pixel(x0, x1) || !holds_pixel(y0, y1);
                    const bool whole = x0 <= 20 * 16 && x1 >= 620 * 16 && y0 <= 20 * 16 && y1 >= 420 * 16;
                    if (!whole && !too_small && ui_squeeze::percent() < 100) {
                        const int pct = ui_squeeze::percent();
                        // the fixed point: the left edge, the middle or the right edge of the screen, by the group the shape is in
                        bool visible = false;
                        for (int i = 0; i < needed; ++i) visible = visible || s.queue[i].a > 4;
                        // The game hides and reveals panels with a stencil: a shape that writes only the picture's alpha bit
                        // (every colour bit masked off) marks an area, and the panel's pieces are then drawn with the
                        // destination alpha test, so they show only inside that area. In build mode the buy catalogue's
                        // frame is still drawn that way, entirely outside its area: nothing of it shows, but its box filled
                        // the screen. So: the stencil shape itself marks nothing, and a tested shape marks only the part of
                        // its box that lies inside the last stencil area.
                        static int mask_x0 = 0, mask_y0 = 0, mask_x1 = 0, mask_y1 = 0;
                        bool inside_mask = false;  // a tested shape with some of its box inside the stencil area
                        int bx0 = x0 >> 4, by0 = y0 >> 4, bx1 = x1 >> 4, by1 = y1 >> 4;
                        // Only the part inside the scissor can show, so only that part counts. The sand of the busy hourglass
                        // is a strip ~435 pixels wide that slides through the glass, clipped to the glass by the scissor: by
                        // its whole box its middle was off the screen's right edge, where nothing is anchored, so the sand
                        // went to the middle (x ~510 of 640) while the glass stayed at the edge, until the strip had slid
                        // far enough left (~15 frames each time the hourglass appears).
                        {
                            const int sx0 = static_cast<int>(c.scissor & 0x7FFu), sx1 = static_cast<int>((c.scissor >> 16) & 0x7FFu);
                            const int sy0 = static_cast<int>((c.scissor >> 32) & 0x7FFu), sy1 = static_cast<int>((c.scissor >> 48) & 0x7FFu);
                            const int cx0 = std::max(bx0, sx0), cx1 = std::min(bx1, sx1), cy0 = std::max(by0, sy0), cy1 = std::min(by1, sy1);
                            if (cx0 <= cx1 && cy0 <= cy1) bx0 = cx0, bx1 = cx1, by0 = cy0, by1 = cy1;
                        }
                        if (((c.frame >> 32) & 0x00FFFFFFu) == 0x00FFFFFFu) {
                            mask_x0 = bx0; mask_y0 = by0; mask_x1 = bx1; mask_y1 = by1;
                            visible = false;
                        } else if ((c.test >> 14) & 1u) {
                            bx0 = std::max(bx0, mask_x0); by0 = std::max(by0, mask_y0); bx1 = std::min(bx1, mask_x1); by1 = std::min(by1, mask_y1);
                            inside_mask = bx0 < bx1 && by0 < by1;
                            if (!inside_mask) {
                                visible = false;
                                bx0 = x0 >> 4; by0 = y0 >> 4; bx1 = x1 >> 4; by1 = y1 >> 4;
                            }
                        }
                        int anchor = ui_squeeze::note(bx0, by0, bx1, by1, visible);
                        // A piece drawn with that test goes where its stencil area goes, wherever it is itself. The lists of
                        // clothes and objects also draw the columns of the pages before and after, to the left and right of the
                        // area, where the test hides them. Such a column is outside the panel, so by its own place it went to the
                        // middle of the screen while the area went to the left edge with the panel: it landed inside the area,
                        // and the pictures of the page before showed over the list or in its empty places.
                        // local change (the busy hourglass): a tested piece with some of itself inside the area goes where that part
                        // is (the anchor note() gave the clipped box); only one entirely outside the area follows the stencil. And the
                        // stencil shape goes where the pieces tested against it went at the last refresh (stencils of the same rows of
                        // the screen are taken as the same stencil), else where the cells under it agree, else the middle. The
                        // hourglass's sand is drawn through a stencil strip ~500 pixels wide that slides through the glass: by the
                        // strip's middle, strip and sand went to the middle of the screen (x ~510 of 640) while the glass stayed at
                        // the right edge, ~15 frames each time the hourglass appears (user's screenshot, house.state 12377-12391).
                        static int mask_anchor = 0, mask_band0 = -1, mask_band1 = -1;
                        static std::uint64_t mask_frame = ~0ull;
                        struct Band {
                            int y0, y1, anchor;
                            std::uint64_t frame;
                        };
                        static std::vector<Band> bands;  // a stencil's rows -> where its pieces went, and when
                        const std::uint64_t now = ps2::frames_run();
                        if (((c.frame >> 32) & 0x00FFFFFFu) == 0x00FFFFFFu) {
                            int chosen = -1;
                            for (const Band &b : bands)
                                if (b.y0 == by0 && b.y1 == by1 && b.frame + 6 >= now) chosen = b.anchor;
                            anchor = chosen >= 0 ? chosen : ui_squeeze::area_anchor(bx0, by0, bx1, by1);
                            mask_anchor = anchor, mask_frame = now, mask_band0 = by0, mask_band1 = by1;
                        } else if (((c.test >> 14) & 1u) && mask_frame == now) {
                            if (!inside_mask) anchor = mask_anchor;
                            else {
                                bool known = false;
                                for (Band &b : bands)
                                    if (b.y0 == mask_band0 && b.y1 == mask_band1) b.anchor = anchor, b.frame = now, known = true;
                                if (!known) {
                                    if (bands.size() >= 64) bands.erase(bands.begin());
                                    bands.push_back({mask_band0, mask_band1, anchor, now});
                                }
                            }
                        }
                        const int fixed_px = anchor == 1 ? 0 : anchor == 2 ? 640 : 320;
                        const int fixed = ofx + fixed_px * 16;
                        for (int i = 0; i < needed; ++i) {
                            narrowed[i] = s.queue[i];
                            // Frames and panels are laid out on the console's half-pixel grid (x.5), so that every pixel reads the
                            // middle of a texel. Narrowed, their edges fell between pixels and single lines of the finer samples
                            // read the outermost texels of a piece: thin dark lines at the corners of dialog frames. A corner
                            // that was on that grid goes to the nearest place on it again; text (anywhere between) is left alone.
                            const int from = static_cast<int>(s.queue[i].x) - fixed;
                            int to = from * pct / 100;
                            if (((static_cast<int>(s.queue[i].x) - ofx) & 15) == 8) to = (to >= 0 ? to / 16 : -((-to + 15) / 16)) * 16 + 8;
                            narrowed[i].x = static_cast<std::uint32_t>(fixed + to);
                        }
                        out = narrowed;
                        const int sx0 = static_cast<int>(c.scissor & 0x7FFu), sx1 = static_cast<int>((c.scissor >> 16) & 0x7FFu);
                        if (sx0 > 0 || sx1 < 639) {
                            const int nx0 = fixed_px + (sx0 - fixed_px) * pct / 100, nx1 = fixed_px + (sx1 + 1 - fixed_px) * pct / 100 - 1;
                            old_scissor = c.scissor;
                            changed = &c;
                            c.scissor = (c.scissor & ~0x07FF07FFull) | static_cast<std::uint64_t>(std::clamp(nx0, 0, 2047)) | (static_cast<std::uint64_t>(std::clamp(nx1, 0, 2047)) << 16);
                            ++g_setup_generation;
                        }
                    }
                }
            }
            if (!too_small) {
                if (cpu_raster()) raster::draw(s, out, needed);
#ifdef PS2_GPU
                if (gpu_on()) gpu::draw(s, out, needed);
#endif
            }
            if (changed) {
                changed->scissor = old_scissor;
                ++g_setup_generation;
            }
        }
        ++g_primitives;
    }
    switch (prim) {
    case 2:
        s.queue[0] = s.queue[1];
        s.count = 1;
        break;
    case 4:
        s.queue[0] = s.queue[1];
        s.queue[1] = s.queue[2];
        s.count = 2;
        break;
    case 5:
        s.queue[1] = s.queue[2];
        s.count = 2;
        break;
    default:
        s.count = 0;
        break;
    }
}

[[gnu::always_inline]] static inline void vertex(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint8_t f, bool draw) {
    State &s = g_state;
    if (s.count >= 3) s.count = 2;
    Vertex &v = s.queue[s.count++];
    v.x = x;
    v.y = y;
    v.z = z;
    v.f = f;
    v.r = static_cast<std::uint8_t>(s.rgbaq);
    v.g = static_cast<std::uint8_t>(s.rgbaq >> 8);
    v.b = static_cast<std::uint8_t>(s.rgbaq >> 16);
    v.a = static_cast<std::uint8_t>(s.rgbaq >> 24);
    std::uint32_t qbits = static_cast<std::uint32_t>(s.rgbaq >> 32), sbits = static_cast<std::uint32_t>(s.st), tbits = static_cast<std::uint32_t>(s.st >> 32);
    std::memcpy(&v.q, &qbits, 4);
    std::memcpy(&v.s, &sbits, 4);
    std::memcpy(&v.t, &tbits, 4);
    v.u = static_cast<std::uint32_t>(s.uv) & 0x3FFFu;
    v.v = static_cast<std::uint32_t>(s.uv >> 16) & 0x3FFFu;
    kick(draw);
}

static const char *const g_gsseq = std::getenv("PS2_TRACE_GSSEQ");

void write(std::uint32_t reg, std::uint64_t value) {
    State &s = g_state;
    ++g_writes[reg & 127u];
    constexpr std::uint64_t kVertexRegs = (1ull << 0x01) | (1ull << 0x02) | (1ull << 0x03) | (1ull << 0x04) | (1ull << 0x05) | (1ull << 0x0A) | (1ull << 0x0C) | (1ull << 0x0D);
    if (reg >= 64 || !((kVertexRegs >> reg) & 1u)) ++g_setup_generation;
    if (g_gsseq) [[unlikely]] {
        static std::uint64_t n = 0, start = std::strtoull(g_gsseq, nullptr, 10);
        if (++n >= start && n < start + 100000) std::fprintf(stderr, "[gs] w %02X %016llX\n", reg, static_cast<unsigned long long>(value));
    }
    switch (reg) {
    case 0x00:
        s.prim = value;
        s.count = 0;
        break;
    case 0x01: s.rgbaq = value; break;
    case 0x02: s.st = value; break;
    case 0x03: s.uv = value; break;
    case 0x04: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32) & 0xFFFFFFu, static_cast<std::uint8_t>(value >> 56), true); break;
    case 0x05: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32), static_cast<std::uint8_t>(s.fog >> 56), true); break;
    case 0x06: case 0x07:
        s.ctx[reg - 6].tex0 = value;
        load_clut(value);
        break;
    case 0x08: case 0x09: s.ctx[reg - 8].clamp = value; break;
    case 0x0A: s.fog = value; break;
    case 0x0C: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32) & 0xFFFFFFu, static_cast<std::uint8_t>(value >> 56), false); break;
    case 0x0D: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32), static_cast<std::uint8_t>(s.fog >> 56), false); break;
    case 0x14: case 0x15: s.ctx[reg - 0x14].tex1 = value; break;
    case 0x16: case 0x17: {
        Context &c = s.ctx[reg - 0x16];
        const std::uint64_t mask = 0xFFFFFFE0'03F00000ull;
        c.tex0 = (c.tex0 & ~mask) | (value & mask);
        load_clut(c.tex0);
        break;
    }
    case 0x18: case 0x19: s.ctx[reg - 0x18].xyoffset = value; break;
    case 0x1A: s.prmodecont = value; break;
    case 0x1B: s.prmode = value; break;
    case 0x1C: s.texclut = value; break;
    case 0x22: s.scanmsk = value; break;
    case 0x34: case 0x35: s.ctx[reg - 0x34].miptbp1 = value; break;
    case 0x36: case 0x37: s.ctx[reg - 0x36].miptbp2 = value; break;
    case 0x3B: s.texa = value; break;
    case 0x3D: s.fogcol = value; break;
    case 0x3F:
#ifdef PS2_GPU
        if (gpu_on()) gpu::texflush();
#endif
        break;
    case 0x40: case 0x41: s.ctx[reg - 0x40].scissor = value; break;
    case 0x42: case 0x43: s.ctx[reg - 0x42].alpha = value; break;
    case 0x44: s.dimx = value; break;
    case 0x45: s.dthe = value; break;
    case 0x46: s.colclamp = value; break;
    case 0x47: case 0x48: s.ctx[reg - 0x47].test = value; break;
    case 0x49: s.pabe = value; break;
    case 0x4A: case 0x4B: s.ctx[reg - 0x4A].fba = value; break;
    case 0x4C: case 0x4D: s.ctx[reg - 0x4C].frame = value; break;
    case 0x4E: case 0x4F: s.ctx[reg - 0x4E].zbuf = value; break;
    case 0x50: s.bitbltbuf = value; break;
    case 0x51: s.trxpos = value; break;
    case 0x52: s.trxreg = value; break;
    case 0x53:
        if (trace_display()) {
            std::fprintf(stderr, "[gs] trx dir %llu buf 0x%016llX pos 0x%016llX reg 0x%016llX\n", static_cast<unsigned long long>(value & 3u),
                         static_cast<unsigned long long>(s.bitbltbuf), static_cast<unsigned long long>(s.trxpos), static_cast<unsigned long long>(s.trxreg));
        }
        trace::transfer(s, value);
        s.trxdir = value;
        ++texdump::g_uploads;
        g_transfer = Transfer{};
        g_carried = 0;
#ifdef PS2_GPU
        if (gpu_on()) {
            if ((value & 3u) == 0) gpu::image_begin(s);
            else if ((value & 3u) == 2) gpu::local_copy(s);
            else if ((value & 3u) == 1 && !cpu_raster()) download_source(s);
        }
#endif
        if ((value & 3u) == 0) g_transfer.active = true;
        else if ((value & 3u) == 2) { if (cpu_raster()) local_copy(); }
        else if ((value & 3u) == 1) g_transfer.active = true;
        break;
    case 0x54:
        feed_image(reinterpret_cast<const std::uint8_t *>(&value), 8);
        break;
    case 0x60: case 0x61: case 0x62:
        if (!queue::active()) signal(reg, value);
        break;
    default:
        break;
    }
}

// A game that counts FINISH events (Metal Gear Solid 3) would lose those that land while CSR.FINISH is still set from the one
// before: they merge into one. So a FINISH that arrives with the bit set waits and is raised, one by one, as the game clears the
// bit (follows upstream 75ccb0b, where the FINISH is raised at the write itself).
static bool trace_finish() {
    static const bool on = !ps2::os::env("PS2_TRACE_FINISH").empty();   // debugging: one line for every FINISH event and every write to CSR
    return on;
}

void raise_finish() {
    if (trace_finish()) std::fprintf(stderr, "[gs] FINISH event f%llu csr %llX imr %llX pending %u\n", static_cast<unsigned long long>(frame_count()),
                                     static_cast<unsigned long long>(g_csr), static_cast<unsigned long long>(g_imr), g_finish_pending);
    if (g_csr & 2u) ++g_finish_pending;
    else raise(2);
    if (g_finish_queued) {
        --g_finish_queued;
        schedule(hw::kEventGsFinish, now() + kFinishDelay, raise_finish);
    }
}

void signal(std::uint32_t reg, std::uint64_t value) {
    const std::uint32_t mask = static_cast<std::uint32_t>(value >> 32), id = static_cast<std::uint32_t>(value);
    switch (reg) {
    case 0x60:
        g_siglblid = (g_siglblid & ~static_cast<std::uint64_t>(mask)) | (id & mask);
        raise(1);
        break;
    case 0x61:
        // The GS only gets to FINISH after it has drawn what came before, so it must not be visible the
        // moment the DMA starts: ps2sdk's graph_wait_vsync writes CSR back right after starting the DMA,
        // and a FINISH that is already set would be wiped before draw_wait_finish ever sees it.
        // The scheduler keeps one event per id, so a second FINISH written before the first has fired would replace it and one event
        // would be lost; a game that counts them (Metal Gear Solid 3 sends three in a row and waits for all three) then waits for ever.
        // The later ones wait in g_finish_queued and follow one kFinishDelay apart.
        if (scheduled_at(hw::kEventGsFinish) != UINT64_MAX) {
            ++g_finish_queued;
            if (trace_finish()) std::fprintf(stderr, "[gs] FINISH write f%llu queued behind an earlier one (%u waiting)\n",
                                             static_cast<unsigned long long>(frame_count()), g_finish_queued);
        } else schedule(hw::kEventGsFinish, now() + kFinishDelay, raise_finish);
        break;
    case 0x62: {
        const std::uint64_t label = (g_siglblid >> 32 & ~static_cast<std::uint64_t>(mask)) | (id & mask);
        g_siglblid = (g_siglblid & 0xFFFFFFFFull) | (label << 32);
        break;
    }
    default: break;
    }
}

static void write_packed_generic(std::uint32_t reg, const Reg128 &q);

// The register writes of the PACKED mode that make up almost every shape (colour, texture coordinates, vertices) go straight to the code that
// stores them; write() would unpack the vertex words into one 64-bit value just for vertex() to take them apart again. It counts the register
// in g_writes (statistics) and does not change g_setup_generation for these registers, as write() does. The rest, and everything while
// PS2_TRACE_GSSEQ prints the writes, goes the old way.
[[gnu::always_inline]] static inline void packed_one(std::uint32_t reg, const Reg128 &q) {
    State &s = g_state;
    switch (reg) {
    case 0x1: {
        std::uint32_t qbits;
        std::memcpy(&qbits, &s.q, 4);
        s.rgbaq = (q.w[0] & 0xFFu) | ((q.w[1] & 0xFFu) << 8) | ((q.w[2] & 0xFFu) << 16) | ((q.w[3] & 0xFFu) << 24) |
                  (static_cast<std::uint64_t>(qbits) << 32);
        break;
    }
    case 0x2:
        s.st = q.d[0];
        std::memcpy(&s.q, &q.w[2], 4);
        break;
    case 0x3: s.uv = (q.w[0] & 0x3FFFu) | ((q.w[1] & 0x3FFFu) << 16); break;
    case 0x4: {
        const bool adc = (q.d[1] >> 47) & 1u;
        ++g_writes[adc ? 0x0C : 0x04];
        vertex(q.w[0] & 0xFFFFu, q.w[1] & 0xFFFFu, (q.w[2] >> 4) & 0xFFFFFFu, static_cast<std::uint8_t>((q.w[3] >> 4) & 0xFFu), !adc);
        break;
    }
    case 0x5: {
        const bool adc = (q.d[1] >> 47) & 1u;
        ++g_writes[adc ? 0x0D : 0x05];
        vertex(q.w[0] & 0xFFFFu, q.w[1] & 0xFFFFu, q.w[2], static_cast<std::uint8_t>(s.fog >> 56), !adc);
        break;
    }
    default: write_packed_generic(reg, q); break;
    }
}

void write_packed(std::uint32_t reg, const Reg128 &q) {
    if (g_gsseq) [[unlikely]] {
        write_packed_generic(reg, q);
        return;
    }
    packed_one(reg, q);
}

// `count` qwords of PACKED data in a row, the first for descriptor `reg` of `regs` (`nreg` of them): the same writes in the same order as
// write_packed for each one, but one call for the run instead of one per qword. On the God of War ship (about 53,000 small triangles a
// frame, nine qwords each) the calls, the trace check and the reloads around them took about 0.3 ms of the drawing thread a frame.
void write_packed_run(const std::uint8_t *data, std::uint32_t count, std::uint64_t regs, std::uint32_t nreg, std::uint32_t reg) {
    for (std::uint32_t k = 0; k < count; ++k) {
        Reg128 q;
        std::memcpy(&q, data + static_cast<std::size_t>(k) * 16, 16);
        const std::uint32_t r = static_cast<std::uint32_t>(regs >> (reg * 4)) & 0xFu;
        if (g_gsseq) [[unlikely]] write_packed_generic(r, q);
        else packed_one(r, q);
        if (++reg >= nreg) reg = 0;
    }
}

static void write_packed_generic(std::uint32_t reg, const Reg128 &q) {
    State &s = g_state;
    static const char *const seq = std::getenv("PS2_TRACE_GSSEQ");
    if (seq) [[unlikely]] {
        static std::uint64_t n = 0, start = std::strtoull(seq, nullptr, 10);
        if (++n >= start && n < start + 100000) std::fprintf(stderr, "[gs] w %02X %016llX\n", reg, static_cast<unsigned long long>(q.d[0]));
    }
    switch (reg) {
    case 0x0: write(0x00, q.d[0] & 0x7FFu); break;
    case 0x1: {
        std::uint32_t qbits;
        std::memcpy(&qbits, &s.q, 4);
        s.rgbaq = (q.w[0] & 0xFFu) | ((q.w[1] & 0xFFu) << 8) | ((q.w[2] & 0xFFu) << 16) | ((q.w[3] & 0xFFu) << 24) |
                  (static_cast<std::uint64_t>(qbits) << 32);
        break;
    }
    case 0x2:
        s.st = q.d[0];
        std::memcpy(&s.q, &q.w[2], 4);
        break;
    case 0x3: s.uv = (q.w[0] & 0x3FFFu) | ((q.w[1] & 0x3FFFu) << 16); break;
    case 0x4: {
        const std::uint64_t v = (q.w[0] & 0xFFFFu) | (static_cast<std::uint64_t>(q.w[1] & 0xFFFFu) << 16) |
                                (static_cast<std::uint64_t>((q.w[2] >> 4) & 0xFFFFFFu) << 32) |
                                (static_cast<std::uint64_t>((q.w[3] >> 4) & 0xFFu) << 56);
        write(((q.d[1] >> 47) & 1u) ? 0x0C : 0x04, v);
        break;
    }
    case 0x5: {
        const std::uint64_t v = (q.w[0] & 0xFFFFu) | (static_cast<std::uint64_t>(q.w[1] & 0xFFFFu) << 16) | (static_cast<std::uint64_t>(q.w[2]) << 32);
        write(((q.d[1] >> 47) & 1u) ? 0x0D : 0x05, v);
        break;
    }
    case 0xA: s.fog = static_cast<std::uint64_t>((q.w[3] >> 4) & 0xFFu) << 56; break;
    case 0xC: write(0x0C, (q.w[0] & 0xFFFFu) | (static_cast<std::uint64_t>(q.w[1] & 0xFFFFu) << 16) |
                              (static_cast<std::uint64_t>((q.w[2] >> 4) & 0xFFFFFFu) << 32) |
                              (static_cast<std::uint64_t>((q.w[3] >> 4) & 0xFFu) << 56)); break;
    case 0xD: write(0x0D, (q.w[0] & 0xFFFFu) | (static_cast<std::uint64_t>(q.w[1] & 0xFFFFu) << 16) | (static_cast<std::uint64_t>(q.w[2]) << 32)); break;
    case 0xE: write(static_cast<std::uint32_t>(q.d[1] & 0xFFu), q.d[0]); break;
    case 0xF: break;
    default: write(reg, q.d[0]); break;
    }
}

void write_image(const std::uint8_t *data, std::uint32_t qwords) { feed_image(data, static_cast<std::size_t>(qwords) * 16); }

std::atomic<std::uint32_t> g_downloads{0};

void read_image(std::uint8_t *data, std::uint32_t qwords) {
    if (!queue::active()) {
        render_read_image(data, qwords);
        return;
    }
    DownloadKey key = g_shadow;
    key.qwords = qwords;
    g_shadow.offset += qwords * 16;
    const std::size_t bytes = static_cast<std::size_t>(qwords) * 16;
    // A depth buffer is always read as it is now: a game that reads its depth and writes it back (Digital Devil Saga does it in
    // every frame) would otherwise put the depth of the frame before back and draw against it (lights that are tested against
    // the ceiling go missing while the camera moves).
    const std::uint32_t spsm = static_cast<std::uint32_t>(key.buf >> 24) & 0x3Fu;
    const bool depth = spsm == PSMZ32 || spsm == PSMZ24 || spsm == PSMZ16 || spsm == PSMZ16S;
    if (!depth) {
        std::lock_guard lock(g_cache_mutex);
        const auto it = std::find_if(g_download_cache.begin(), g_download_cache.end(), [&](const CachedDownload &c) { return c.key == key; });
        if (it != g_download_cache.end() && it->data.size() == bytes && frames_run() - it->frame <= kCacheFrames) {
            std::memcpy(data, it->data.data(), bytes);
            it->used = ++g_cache_clock;
            it->frame = frames_run();
            queue::post(queue::kDownloadAsync, qwords, 0, &key, sizeof key);
            return;
        }
    }
    const std::uint32_t seen = g_downloads.load();
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(data);
    queue::post(queue::kDownload, qwords, 0, &target, sizeof target);
    while (g_downloads.load() == seen) queue::wait(g_downloads, seen);
    remember(key, data, bytes);
}

void note_transfer(std::uint32_t reg, std::uint64_t value) {
    switch (reg) {
    case 0x50: g_shadow.buf = value; break;
    case 0x51: g_shadow.pos = value; break;
    case 0x52: g_shadow.reg = value; break;
    default: g_shadow.offset = 0; break;
    }
}

void render_read_image(std::uint8_t *data, std::uint32_t qwords) {
    const std::size_t bytes = static_cast<std::size_t>(qwords) * 16;
    std::memset(data, 0, bytes);
    if (!g_transfer.active || (g_state.trxdir & 3u) != 1) return;
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t sbp = static_cast<std::uint32_t>(buf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(buf >> 16) & 0x3Fu;
    const std::uint32_t spsm = static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
    const std::uint32_t ssax = static_cast<std::uint32_t>(pos) & 0x7FFu, ssay = static_cast<std::uint32_t>(pos >> 16) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    const std::uint32_t bits = bits_per_pixel(spsm);
    auto next = [&]() {
        const std::uint32_t v = read_pixel(spsm, sbp, sbw, (ssax + g_transfer.x) & 0x7FFu, (ssay + g_transfer.y) & 0x7FFu);
        if (++g_transfer.x >= rrw) {
            g_transfer.x = 0;
            if (++g_transfer.y >= rrh) g_transfer.active = false;
        }
        return v;
    };
    if (bits == 24) {
        for (std::size_t i = 0; i < bytes && (g_transfer.active || g_carried); ++i) {
            if (!g_carried) {
                const std::uint32_t v = next();
                g_carry[0] = static_cast<std::uint8_t>(v);
                g_carry[1] = static_cast<std::uint8_t>(v >> 8);
                g_carry[2] = static_cast<std::uint8_t>(v >> 16);
                g_carried = 3;
            }
            data[i] = g_carry[3 - g_carried--];
        }
        return;
    }
    for (std::size_t bit = 0; bit + bits <= bytes * 8 && g_transfer.active; bit += bits) {
        const std::uint32_t v = next();
        if (bits == 4) data[bit / 8] |= static_cast<std::uint8_t>((v & 15u) << (bit & 4));
        else std::memcpy(data + bit / 8, &v, bits / 8);
    }
}

std::uint64_t read_privileged(std::uint32_t address) {
    switch (address & 0x1FFFu) {
    case 0x1000: return g_csr | (g_odd_field ? 0x2000u : 0u);
    case 0x1010: return g_imr;
    case 0x1040: return g_busdir;
    case 0x1080: return g_siglblid;
    default: return 0;
    }
}

void write_privileged(std::uint32_t address, std::uint64_t value) {
    switch (address & 0x1FF0u) {
    case 0x0000:
        if (trace_display() && value != g_pmode) std::fprintf(stderr, "[gs] pmode 0x%llX\n", static_cast<unsigned long long>(value));
        g_pmode = value;
        break;
    case 0x0010: g_smode1 = value; break;
    case 0x0020:
        if (trace_display() && value != g_smode2) std::fprintf(stderr, "[gs] smode2 0x%llX\n", static_cast<unsigned long long>(value));
        g_smode2 = value;
        break;
    case 0x0030: g_srfsh = value; break;
    case 0x0040: g_synch1 = value; break;
    case 0x0050: g_synch2 = value; break;
    case 0x0060: g_syncv = value; break;
    case 0x0070:
        if (value != g_dispfb[0]) {
            if (trace_display()) std::fprintf(stderr, "[gs] dispfb1 0x%llX f%llu\n", static_cast<unsigned long long>(value), static_cast<unsigned long long>(frame_count()));
            kernel::pace_picture();  // a new picture shown (the end-of-run count; the hold of a game with a picture pace)
        }
        g_dispfb[0] = value;
        break;
    case 0x0080:
        if (trace_display() && value != g_display[0]) std::fprintf(stderr, "[gs] display1 0x%llX\n", static_cast<unsigned long long>(value));
        g_display[0] = value;
        break;
    case 0x0090:
        if (value != g_dispfb[1]) {
            if (trace_display()) std::fprintf(stderr, "[gs] dispfb2 0x%llX f%llu\n", static_cast<unsigned long long>(value), static_cast<unsigned long long>(frame_count()));
            kernel::pace_picture();  // a new picture shown (the end-of-run count; the hold of a game with a picture pace)
        }
        g_dispfb[1] = value;
        break;
    case 0x00A0:
        if (trace_display() && value != g_display[1]) std::fprintf(stderr, "[gs] display2 0x%llX\n", static_cast<unsigned long long>(value));
        g_display[1] = value;
        break;
    case 0x00B0: g_extbuf = value; break;
    case 0x00C0: g_extdata = value; break;
    case 0x00D0: g_extwrite = value; break;
    case 0x00E0: g_bgcolor = value; break;
    case 0x1000:
        if (trace_finish()) std::fprintf(stderr, "[gs] CSR write %llX f%llu csr %llX pending %u\n", static_cast<unsigned long long>(value),
                                         static_cast<unsigned long long>(frame_count()), static_cast<unsigned long long>(g_csr), g_finish_pending);
        g_csr &= ~(value & 0x1Fu);
        if ((value & 2u) && g_finish_pending) {
            --g_finish_pending;
            raise(2);
        }
        if (value & 0x200u) {
            g_finish_pending = 0;
            g_finish_queued = 0;
            g_csr = 0x551B4000ull;
            if (queue::active()) queue::post(queue::kSoftReset, 0, 0, nullptr, 0);
            else soft_reset();
        }
        break;
    case 0x1010:
        g_imr = value;
        if (g_csr & ~(g_imr >> 8) & 0x1Fu) hw::intc_raise(0);
        break;
    case 0x1040: g_busdir = value; break;
    case 0x1080: g_siglblid = value; break;
    default: break;
    }
}

void vblank(bool start) {
    if (start) {
        g_odd_field = !g_odd_field;
        raise(8);
        if (queue::active()) queue::post(queue::kVblank, static_cast<std::uint32_t>(frame_count()), 0, nullptr, 0);
        else {
            trace::frame_start(frame_count());
#ifdef PS2_GPU
            if (gpu_on()) gpu::flush();
#endif
        }
    }
}

bool interlaced_odd_field() { return g_odd_field; }

bool attach_window(int kind, void *display, std::uint64_t window, void *surface, const char *selector, std::uint32_t width, std::uint32_t height) {
#ifdef PS2_GPU
    if (gpu::mode() != gpu::Mode::primary) return false;
    const gpu::SurfaceTarget target{static_cast<gpu::SurfaceTarget::Kind>(kind), display, window, surface, selector, width, height};
    return gpu::attach_surface(target);
#else
    (void)kind, (void)display, (void)window, (void)surface, (void)selector, (void)width, (void)height;
    return false;
#endif
}

void resize_window(std::uint32_t width, std::uint32_t height) {
#ifdef PS2_GPU
    gpu::resize_surface(width, height);
#else
    (void)width, (void)height;
#endif
}

double gpu_busy_ms() {
#ifdef PS2_GPU
    return gpu::busy_ms();
#else
    return 0.0;
#endif
}

DisplayRegs display_regs() { return DisplayRegs{g_pmode, g_smode2, {g_dispfb[0], g_dispfb[1]}, {g_display[0], g_display[1]}}; }

bool present() { return present(display_regs()); }

bool present(const DisplayRegs &r) {
#ifdef PS2_GPU
    if (!gpu::has_surface()) return false;
    const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
    if (circuit < 0) return true;
    const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1;
    const std::uint32_t width = ((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / magh, height = display_height(disp, r.smode2);
    if (!width || !height || width > 2048 || height > 2048) return true;
    const gpu::DisplaySource src{(static_cast<std::uint32_t>(fb) & 0x1FFu) * 32, static_cast<std::uint32_t>(fb >> 9) & 0x3Fu, static_cast<std::uint32_t>(fb >> 15) & 0x1Fu,
                                 static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, static_cast<std::uint32_t>(fb >> 43) & 0x7FFu, width, height, (r.smode2 & 3u) == 3u, display_overlay(r.pmode, r.smode2, r.dispfb[0], r.display[0], r.display[1])};
    return gpu::present(src);
#else
    return false;
#endif
}

bool display(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return display(display_regs(), rgba, width, height); }

bool display(const DisplayRegs &r, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
    if (circuit < 0) return false;
    const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
    const std::uint32_t fbp = static_cast<std::uint32_t>(fb) & 0x1FFu, fbw = static_cast<std::uint32_t>(fb >> 9) & 0x3Fu;
    const std::uint32_t psm = static_cast<std::uint32_t>(fb >> 15) & 0x1Fu;
    const std::uint32_t dbx = static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, dby = static_cast<std::uint32_t>(fb >> 43) & 0x7FFu;
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1;
    const std::uint32_t dw = (static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1;
    width = dw / magh;
    height = display_height(disp, r.smode2);
    if (!width || !height || width > 2048 || height > 2048) return false;
    const bool halved = (r.smode2 & 3u) == 3u;
#ifdef PS2_GPU
    if (gpu_on()) {
        const gpu::DisplaySource src{fbp * 32, fbw, psm, dbx, dby, width, height, halved, display_overlay(r.pmode, r.smode2, r.dispfb[0], r.display[0], r.display[1])};
        if (!cpu_raster()) return gpu::display(src, rgba, width, height);
        static const bool compare = std::getenv("PS2_GS_COMPARE") != nullptr;
        std::uint32_t gw = 0, gh = 0;
        if (compare) g_gpu_frame.clear(), gpu::display(src, g_gpu_frame, gw, gh);
    }
#endif
    rgba.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t c = read_pixel(psm, fbp * 32, fbw, dbx + x, dby + (halved ? y / 2 : y));
            if (psm == PSMCT16 || psm == PSMCT16S)
                c = ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9) | 0xFF000000u;
            rgba[static_cast<std::size_t>(y) * width + x] = c | 0xFF000000u;
        }
    if (const DisplayOverlay o = display_overlay(r.pmode, r.smode2, r.dispfb[0], r.display[0], r.display[1]); o.active) {
        const std::uint32_t ofbp = o.fbp, ofbw = o.fbw, opsm = o.psm;
        for (std::uint32_t y = 0; y < o.height; ++y) {
            const std::int32_t py = o.y + static_cast<std::int32_t>(y);
            if (py < 0 || py >= static_cast<std::int32_t>(height)) continue;
            for (std::uint32_t x = 0; x < o.width; ++x) {
                const std::int32_t px = o.x + static_cast<std::int32_t>(x);
                if (px < 0 || px >= static_cast<std::int32_t>(width)) continue;
                std::uint32_t c = read_pixel(opsm, ofbp, ofbw, o.dbx + x, o.dby + (halved ? y / 2 : y));
                if (opsm == PSMCT16 || opsm == PSMCT16S) c = ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9);
                rgba[static_cast<std::size_t>(py) * width + static_cast<std::uint32_t>(px)] = c | 0xFF000000u;
            }
        }
    }
#ifdef PS2_GPU
    if (!g_gpu_frame.empty() && g_gpu_frame.size() == rgba.size()) {
        std::size_t differ = 0;
        int worst = 0;
        for (std::size_t i = 0; i < rgba.size(); ++i) {
            int delta = 0;
            for (int ch = 0; ch < 24; ch += 8)
                delta = std::max(delta, std::abs(static_cast<int>((rgba[i] >> ch) & 0xFFu) - static_cast<int>((g_gpu_frame[i] >> ch) & 0xFFu)));
            if (delta > 2) ++differ;
            worst = std::max(worst, delta);
        }
        std::fprintf(stderr, "[gs] gpu differs in %zu of %zu pixels (max %d)\n", differ, rgba.size(), worst);
        if (std::strcmp(std::getenv("PS2_GS_COMPARE"), "show") == 0) rgba.swap(g_gpu_frame);
    }
#endif
    return true;
}

bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return copy_frame(display_regs(), rgba, width, height); }

bool copy_frame(const DisplayRegs &r, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    static std::vector<std::uint32_t> previous;
    bool got = false;
#ifdef PS2_GPU
    if (gpu_on() && !cpu_raster()) {
        const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
        if (circuit < 0) return false;
        const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
        const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1;
        const std::uint32_t w = ((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / magh, h = display_height(disp, r.smode2);
        if (!w || !h || w > 2048 || h > 2048) return false;
        const gpu::DisplaySource src{(static_cast<std::uint32_t>(fb) & 0x1FFu) * 32, static_cast<std::uint32_t>(fb >> 9) & 0x3Fu, static_cast<std::uint32_t>(fb >> 15) & 0x1Fu,
                                     static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, static_cast<std::uint32_t>(fb >> 43) & 0x7FFu, w, h, (r.smode2 & 3u) == 3u, display_overlay(r.pmode, r.smode2, r.dispfb[0], r.display[0], r.display[1])};
        got = gpu::display_async(src, rgba, width, height);
    } else
#endif
        got = display(r, rgba, width, height);
    if (!got) return false;
    if (previous.size() != rgba.size()) {
        previous = rgba;
        return true;
    }
    static const bool weave_off = std::getenv("PS2_NO_DEINTERLACE") != nullptr;
    const bool blend = (r.smode2 & 3u) == 3u && !weave_off;
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        const std::uint32_t c = rgba[i];
        if (blend) rgba[i] = (c & previous[i]) + (((c ^ previous[i]) & 0xFEFEFEFEu) >> 1);
        previous[i] = c;
    }
    return true;
}

std::atomic<std::uint32_t> g_presented{0};
std::uint32_t g_posted_presents = 0;

void flush_and_wait() {
#ifdef PS2_GPU
    if (gpu_on()) gpu::wait_idle();
#endif
}

// The drawing thread's ring, 8 MiB: the largest message seen in nine games is 256 KiB, anything over
// 2 MiB goes through a heap copy (queue.cpp), and a smaller ring keeps the copy in the cache (Silent Hill 2 6 % faster than with 32 MiB).
// PS2_GS_RING_MB=<n> sets another size (a power of two is taken).
void start_thread() {
    const char *mb = std::getenv("PS2_GS_RING_MB");
    queue::render().start((mb && std::atoi(mb) > 0 ? static_cast<std::size_t>(std::atoi(mb)) : 8u) << 20);
}

std::uint32_t g_present_frame = 0;

std::uint32_t present_frame() { return g_present_frame; }

void post_present() {
    const DisplayRegs regs = display_regs();
    queue::post(queue::kPresent, static_cast<std::uint32_t>(frame_count()), 0, &regs, sizeof regs);
    ++g_posted_presents;
    for (std::uint32_t done = g_presented.load(); g_posted_presents - done > 2; done = g_presented.load()) queue::wait(g_presented, done);
}

Rendered render_pending(const std::function<void(const DisplayRegs &)> &on_present, double budget_ms) {
    queue::role() = queue::Role::Render;
    queue::Header h;
    const std::uint8_t *payload = nullptr;
    const auto start = std::chrono::steady_clock::now();
    // PS2_HITCH_LOG=<ms>: per picture, how long the window thread worked on each kind of message (wall clock and its own CPU time), and any
    // single message that took longer than the limit (docs/hitches.md). The picture's lines are printed when it is handed to the screen.
    static double busy[16] = {};
    static std::uint32_t messages[16] = {}, last_frame = 0;
    static double cpu_at_start = 0;
    const bool hitch_on = hitch::on();
    for (std::uint32_t n = 1; queue::render().peek(h, payload); ++n) {
        if (!(n & 7u) && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budget_ms) return Rendered::busy;
        bool presented = false;
        hitch::Clock::time_point step_start{};
        double step_cpu = 0;
        if (hitch_on) {
            step_start = hitch::Clock::now();
            step_cpu = hitch::thread_cpu_ms();
            if (!cpu_at_start) cpu_at_start = step_cpu;
        }
        switch (h.type) {
        case queue::kGif: gif::render(static_cast<int>(h.arg0), payload, h.arg1); break;
        case queue::kGifReset: gif::render_reset(); break;
        case queue::kReset: render_reset(); break;
        case queue::kSoftReset: soft_reset(); break;
        case queue::kVblank:
            trace::frame_start(h.arg0);
#ifdef PS2_GPU
            if (gpu_on()) gpu::flush();
#endif
            break;
        case queue::kPresent: {
            DisplayRegs regs;
            std::memcpy(&regs, payload, sizeof regs);
            g_present_frame = h.arg0;
            on_present(regs);
            presented = true;
            break;
        }
        case queue::kState: serve_state(payload); break;
        case queue::kDownload: {
            std::uintptr_t target;
            std::memcpy(&target, payload, sizeof target);
            exact_download();
            render_read_image(reinterpret_cast<std::uint8_t *>(target), h.arg0);
            g_downloads.fetch_add(1);
            queue::notify(g_downloads);
            break;
        }
        case queue::kDownloadAsync: {
            DownloadKey key;
            std::memcpy(&key, payload, sizeof key);
            std::vector<std::uint8_t> fresh(static_cast<std::size_t>(h.arg0) * 16);
            render_read_image(fresh.data(), h.arg0);
            if (key.buf == g_state.bitbltbuf && key.pos == g_state.trxpos && key.reg == g_state.trxreg) remember(key, fresh.data(), fresh.size());
            else forget(key);
            break;
        }
        default: break;
        }
        if (hitch_on) {
            const double ms = hitch::ms_since(step_start);
            const unsigned kind = h.type < 16 ? h.type : 15;
            busy[kind] += ms;
            ++messages[kind];
            if (h.type == queue::kVblank) last_frame = h.arg0;
            if (ms > hitch::threshold_ms())
                std::fprintf(stderr, "[hitch] window thread: one message of kind %u (arg %u, %u bytes) took %.1f ms (its CPU time %.1f ms), around frame %u\n", h.type, h.arg0, h.arg1, ms,
                             hitch::thread_cpu_ms() - step_cpu, last_frame);
            if (presented) {
                double total = 0;
                for (const double b : busy) total += b;
                static std::FILE *const table = [] {  // PS2_HITCH_PICTURES=<file>: one line per picture
                    const std::string path = os::env("PS2_HITCH_PICTURES");
                    std::FILE *f = path.empty() ? nullptr : os::open(path.c_str(), "w");
                    if (f) std::fprintf(f, "frame gap_ms gif_msgs gif_ms other_ms flush_ms acquire_ms submit_ms present_ms\n");
                    return f;
                }();
                if (table) {
                    static hitch::Clock::time_point last_picture = hitch::Clock::now();
                    const auto now = hitch::Clock::now();
                    const hitch::PictureTimes &pt = hitch::picture_times();
                    std::fprintf(table, "%u %.2f %u %.2f %.2f %.2f %.2f %.2f %.2f\n", last_frame, std::chrono::duration<double, std::milli>(now - last_picture).count(), messages[queue::kGif],
                                 busy[queue::kGif], total - busy[queue::kGif] - busy[queue::kPresent], pt.flush, pt.acquire, pt.submit, pt.present);
                    last_picture = now;
                }
                if (total > hitch::threshold_ms()) {
                    std::fprintf(stderr, "[hitch] window thread, picture after frame %u: %.1f ms busy (CPU %.1f ms):", last_frame, total, hitch::thread_cpu_ms() - cpu_at_start);
                    static const char *const names[] = {"wrap", "gif", "gif-reset", "reset", "soft-reset", "vblank", "present", "download", "vif1", "vif1-write", "vu-reset", "state", "vu-fence", "download-async", "path3-idle"};
                    for (unsigned k = 0; k < 16; ++k)
                        if (messages[k]) std::fprintf(stderr, " %s %u msgs %.1f ms;", k < sizeof names / sizeof *names ? names[k] : "other", messages[k], busy[k]);
                    std::fprintf(stderr, "\n");
                }
                for (unsigned k = 0; k < 16; ++k) busy[k] = 0, messages[k] = 0;
                cpu_at_start = 0;
            }
        }
        queue::render().pop(h);
        if (presented) {
            g_presented.fetch_add(1);
            queue::notify(g_presented);
            return Rendered::presented;
        }
    }
    return Rendered::idle;
}

std::uint64_t primitives_drawn() { return g_primitives; }

void dump_region(const char *spec) {
#ifdef PS2_GPU
    if (gpu_on() && !cpu_raster()) gpu::download(g_memory);
#endif
    unsigned bp = 0, bw = 0, psm = 0, w = 0, h = 0;
    char path[256] = {};
    if (std::sscanf(spec, "%x,%u,%u,%u,%u,%255s", &bp, &bw, &psm, &w, &h, path) != 6) return;
    std::FILE *f = os::open(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const std::uint32_t c = read_pixel(psm, bp, bw, x, y);
            const std::uint8_t px[3] = {static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c >> 16)};
            std::fwrite(px, 1, 3, f);
        }
    std::fclose(f);
}

// PS2_DUMP_VRAM_RAW=<path>: the 4 MB of graphics memory as the program ends, and its hash on the log.
void dump_raw(const char *path) {
#ifdef PS2_GPU
    if (gpu_on() && !cpu_raster()) gpu::download(g_memory);
#endif
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < kMemorySize; ++i) h = (h ^ g_memory[i]) * 1099511628211ull;
    std::fprintf(stderr, "[gs] graphics memory hash %016llx\n", static_cast<unsigned long long>(h));
    if (std::FILE *f = os::open(path, "wb")) {
        std::fwrite(g_memory, 1, kMemorySize, f);
        std::fclose(f);
    }
}

void stats() {
    if (const std::string spec = os::env("PS2_DUMP_VRAM"); !spec.empty()) dump_region(spec.c_str());
    if (const std::string raw = os::env("PS2_DUMP_VRAM_RAW"); !raw.empty()) dump_raw(raw.c_str());
#ifdef PS2_GPU
    gpu::stats();
#endif
    std::fprintf(stderr, "[gs] %llu primitives drawn, register writes:", static_cast<unsigned long long>(g_primitives));
    for (int r = 0; r < 128; ++r)
        if (g_writes[r]) std::fprintf(stderr, " %02X:%llu", r, static_cast<unsigned long long>(g_writes[r]));
    std::fprintf(stderr, "\n");
}

}

namespace ps2::gs {

namespace {

std::atomic<std::uint32_t> g_state_done{0};
std::string g_state_error;

void render_state(ps2::state::Stream &s) {
    flush_and_wait();
    if (s.loading()) {
        // What was read back from the picture memory of the program that has run on is not the picture memory that is loaded now: the
        // copies kept to answer the next read (and the ranges that are read a frame late) start again, as in a program that was
        // started to load the state.
        {
            std::lock_guard lock(g_cache_mutex);
            g_download_cache.clear();
        }
#ifdef PS2_GPU
        g_seen_ranges.clear();
        g_transfer_stale = false;
#endif
    }
    s.pod(g_state);
    ++g_setup_generation;
    s.pod(g_pmode);
    s.pod(g_smode1);
    s.pod(g_smode2);
    s.pod(g_srfsh);
    s.pod(g_synch1);
    s.pod(g_synch2);
    s.pod(g_syncv);
    s.pod(g_dispfb);
    s.pod(g_display);
    s.pod(g_extbuf);
    s.pod(g_extdata);
    s.pod(g_extwrite);
    s.pod(g_bgcolor);
    s.pod(g_csr);
    s.pod(g_imr);
    s.pod(g_busdir);
    s.pod(g_siglblid);
    if (s.version() >= 9) s.pod(g_finish_pending);
    else g_finish_pending = 0;
    if (s.loading()) g_finish_queued = 0;   // a state keeps the event that is on its way, not the ones queued behind it
    s.pod(g_primitives);
    s.pod(g_odd_field);
    s.pod(g_transfer);
    s.pod(g_carry);
    s.size(g_carried);
    std::vector<std::uint32_t> vram, clut;
    std::uint32_t scale = 1;
#ifdef PS2_GPU
    if (s.saving() && gpu_on()) {
        gpu::download(g_memory);
        gpu::read_state(vram, scale, clut);
    }
#endif
    s.bytes(g_memory, kMemorySize);
    s.pod(scale);
    s.vec(vram);
    s.vec(clut);
#ifdef PS2_GPU
    if (s.loading() && gpu_on()) {
        if (vram.empty()) {
            vram.resize(kMemorySize / 4);
            std::memcpy(vram.data(), g_memory, kMemorySize);
            clut.assign(g_state.clut, g_state.clut + 512);
            scale = 1;
        }
        gpu::write_state(vram, scale, clut);
    }
#endif
}

}

void serve_state(const void *payload) {
    std::uintptr_t target;
    std::memcpy(&target, payload, sizeof target);
    try {
        render_state(*reinterpret_cast<ps2::state::Stream *>(target));
    } catch (const std::exception &e) {
        g_state_error = e.what();
    }
    g_state_done.fetch_add(1);
    queue::notify(g_state_done);
}

void state(ps2::state::Stream &s) {
    s.tag("gs");
    if (!queue::active()) {
        render_state(s);
        return;
    }
    g_state_error.clear();
    const std::uint32_t seen = g_state_done.load();
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(&s);
    queue::post(queue::kState, 0, 0, &target, sizeof target);
    while (g_state_done.load() == seen) queue::wait(g_state_done, seen);
    if (!g_state_error.empty()) throw std::runtime_error(g_state_error);
}

}

// local change (the PC mouse, host/mouse_pc.cpp): whether a full-screen game panel (catalogue, relationships, family, CAS pages) is
// up, as the interface squeeze saw it (only while PS2_UI_SQUEEZE is on)
namespace ps2 {
bool ui_panel_up() { return gs::ui_squeeze::g_panel_up.load(std::memory_order_relaxed); }
}  // namespace ps2
