// Local change (the remaster): PCSX2-style texture packs found by picture (see texpack.hpp).
#include "texpack.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace ps2::texpack {

namespace {

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool read_whole(const std::string &path, std::vector<std::uint8_t> &out) {
    std::FILE *f = os::open(path, "rb");
    if (!f) return false;
    out.clear();
    std::uint8_t chunk[65536];
    std::size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) out.insert(out.end(), chunk, chunk + n);
    std::fclose(f);
    return true;
}

std::uint32_t le32(const std::uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
std::uint32_t be32(const std::uint8_t *p) { return static_cast<std::uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

bool fail(std::string *why, const char *text) {
    if (why) *why = text;
    return false;
}

// ---- DDS (block decoding as Pillow's BcnDecode.c does it, so the thumbnails are the ones tex_match.py saw)

struct Rgba {
    std::uint8_t r, g, b, a;
};

Rgba decode_565(std::uint16_t x) {
    int r = (x & 0xf800) >> 8;
    r |= r >> 5;
    int g = (x & 0x7e0) >> 3;
    g |= g >> 6;
    int b = (x & 0x1f) << 3;
    b |= b >> 5;
    return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b), 0xff};
}

void bc1_colors(const std::uint8_t *src, bool separate_alpha, Rgba out[16]) {
    const std::uint16_t c0 = src[0] | src[1] << 8, c1 = src[2] | src[3] << 8;
    const std::uint32_t lut = le32(src + 4);
    Rgba p[4];
    p[0] = decode_565(c0);
    p[1] = decode_565(c1);
    const int r0 = p[0].r, g0 = p[0].g, b0 = p[0].b, r1 = p[1].r, g1 = p[1].g, b1 = p[1].b;
    if (c0 > c1 || separate_alpha) {
        p[2] = {static_cast<std::uint8_t>((2 * r0 + r1) / 3), static_cast<std::uint8_t>((2 * g0 + g1) / 3), static_cast<std::uint8_t>((2 * b0 + b1) / 3), 0xff};
        p[3] = {static_cast<std::uint8_t>((r0 + 2 * r1) / 3), static_cast<std::uint8_t>((g0 + 2 * g1) / 3), static_cast<std::uint8_t>((b0 + 2 * b1) / 3), 0xff};
    } else {
        p[2] = {static_cast<std::uint8_t>((r0 + r1) / 2), static_cast<std::uint8_t>((g0 + g1) / 2), static_cast<std::uint8_t>((b0 + b1) / 2), 0xff};
        p[3] = {0, 0, 0, 0};
    }
    for (int n = 0; n < 16; ++n) out[n] = p[3 & (lut >> (2 * n))];
}

void bc3_alpha(const std::uint8_t *src, Rgba px[16]) {
    const int a0 = src[0], a1 = src[1];
    std::uint8_t a[8];
    a[0] = static_cast<std::uint8_t>(a0), a[1] = static_cast<std::uint8_t>(a1);
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) a[1 + i] = static_cast<std::uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i <= 4; ++i) a[1 + i] = static_cast<std::uint8_t>(((5 - i) * a0 + i * a1) / 5);
        a[6] = 0, a[7] = 255;
    }
    const std::uint32_t lut1 = src[2] | src[3] << 8 | src[4] << 16, lut2 = src[5] | src[6] << 8 | src[7] << 16;
    for (int n = 0; n < 8; ++n) px[n].a = a[7 & (lut1 >> (3 * n))];
    for (int n = 0; n < 8; ++n) px[8 + n].a = a[7 & (lut2 >> (3 * n))];
}

void bc2_alpha(const std::uint8_t *src, Rgba px[16]) {
    for (int n = 0; n < 16; ++n) {
        const int bit = n * 4;
        int av = 0xf & (src[bit >> 3] >> (bit & 7));
        px[n].a = static_cast<std::uint8_t>(av << 4 | av);
    }
}

int mask_shift(std::uint32_t mask) {
    int s = 0;
    while (mask && !(mask & 1)) mask >>= 1, ++s;
    return s;
}
std::uint32_t mask_value(std::uint32_t pixel, std::uint32_t mask) {
    if (!mask) return 0;
    const int s = mask_shift(mask);
    const std::uint32_t m = mask >> s;
    return ((pixel & mask) >> s) * 255u / m;
}

// ---- inflate (after the zlib header), a small canonical-Huffman decoder in the manner of zlib's puff.c

struct Bits {
    const std::uint8_t *p;
    std::size_t n, pos = 0;
    std::uint32_t buf = 0;
    int cnt = 0;
    bool bad = false;
    int need(int k) {
        while (cnt < k) {
            if (pos >= n) {
                bad = true;
                return 0;
            }
            buf |= static_cast<std::uint32_t>(p[pos++]) << cnt;
            cnt += 8;
        }
        const int v = static_cast<int>(buf & ((1u << k) - 1));
        buf >>= k;
        cnt -= k;
        return v;
    }
};

struct Huffman {
    short count[16];
    short symbol[320];
};

int build(Huffman &h, const short *length, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int s = 0; s < n; ++s) h.count[length[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return -1;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (length[s]) h.symbol[offs[length[s]]++] = static_cast<short>(s);
    return left;
}

int decode(Bits &b, const Huffman &h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= b.need(1);
        if (b.bad) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits &b, std::vector<std::uint8_t> &out, const Huffman &lencode, const Huffman &distcode) {
    for (;;) {
        int symbol = decode(b, lencode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            out.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) return true;
        symbol -= 257;
        if (symbol >= 29) return false;
        const int len = kLenBase[symbol] + b.need(kLenExtra[symbol]);
        const int ds = decode(b, distcode);
        if (ds < 0 || ds >= 30) return false;
        const std::size_t dist = static_cast<std::size_t>(kDistBase[ds] + b.need(kDistExtra[ds]));
        if (b.bad || dist > out.size()) return false;
        const std::size_t from = out.size() - dist;
        for (int i = 0; i < len; ++i) out.push_back(out[from + static_cast<std::size_t>(i)]);
    }
}

// ---- Pillow's resize (BOX) for 8-bit channels

constexpr int kPrecision = 32 - 8 - 2;

struct Coeffs {
    std::vector<int> start, size;
    std::vector<std::int32_t> k;  // ksize per output pixel
    int ksize = 0;
};

Coeffs box_coeffs(int in_size, int out_size) {
    Coeffs c;
    const double scale = static_cast<double>(in_size) / out_size;
    const double filterscale = scale < 1.0 ? 1.0 : scale;
    const double support = 0.5 * filterscale;
    c.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    c.start.resize(static_cast<std::size_t>(out_size));
    c.size.resize(static_cast<std::size_t>(out_size));
    c.k.assign(static_cast<std::size_t>(out_size) * static_cast<std::size_t>(c.ksize), 0);
    std::vector<double> kd(static_cast<std::size_t>(c.ksize));
    for (int xx = 0; xx < out_size; ++xx) {
        const double center = (xx + 0.5) * scale, ss = 1.0 / filterscale;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;
        double ww = 0;
        for (int x = 0; x < xmax; ++x) {
            const double t = (x + xmin - center + 0.5) * ss;
            const double w = t > -0.5 && t <= 0.5 ? 1.0 : 0.0;
            kd[static_cast<std::size_t>(x)] = w;
            ww += w;
        }
        for (int x = 0; x < c.ksize; ++x) {
            double w = x < xmax ? kd[static_cast<std::size_t>(x)] : 0.0;
            if (ww != 0.0) w /= ww;
            c.k[static_cast<std::size_t>(xx) * c.ksize + x] = static_cast<std::int32_t>(w < 0 ? w * (1 << kPrecision) - 0.5 : w * (1 << kPrecision) + 0.5);
        }
        c.start[static_cast<std::size_t>(xx)] = xmin;
        c.size[static_cast<std::size_t>(xx)] = xmax;
    }
    return c;
}

std::uint8_t clip8(std::int64_t v) {
    v >>= kPrecision;
    return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

std::uint8_t muldiv255(int a, int b) {
    const int tmp = a * b + 128;
    return static_cast<std::uint8_t>(((tmp >> 8) + tmp) >> 8);
}

}  // namespace

bool parse_name(const std::string &file_name, Name &out) {
    // PCSX2 writes the numbers without leading zeros, and leaves the palette out for a texture without one:
    // <texture>-<palette>-<flags>.dds or <texture>-<flags>.dds, each 1 to 16 hex digits (flags 8)
    const std::string name = lower(file_name);
    if (name.size() < 5) return false;
    const std::string ext = name.substr(name.size() - 4);
    if (ext != ".dds" && ext != ".png") return false;
    std::vector<std::string> parts;
    std::size_t from = 0;
    const std::string stem = name.substr(0, name.size() - 4);
    for (;;) {
        const std::size_t dash = stem.find('-', from);
        parts.push_back(stem.substr(from, dash == std::string::npos ? std::string::npos : dash - from));
        if (dash == std::string::npos) break;
        from = dash + 1;
    }
    if (parts.size() != 2 && parts.size() != 3) return false;
    for (const std::string &p : parts) {
        if (p.empty() || p.size() > 16) return false;
        for (char c : p)
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    if (parts.back().size() > 8) return false;
    out.texture = std::strtoull(parts[0].c_str(), nullptr, 16);
    out.palette = parts.size() == 3 ? std::strtoull(parts[1].c_str(), nullptr, 16) : 0;
    out.flags = static_cast<std::uint32_t>(std::strtoul(parts.back().c_str(), nullptr, 16));
    out.tw = static_cast<int>((out.flags >> 6) & 15);
    out.th = static_cast<int>((out.flags >> 10) & 15);
    return true;
}

bool decode_dds(const std::uint8_t *d, std::size_t size, std::vector<std::uint32_t> &out, std::uint32_t &w, std::uint32_t &h, std::string *why) {
    if (size < 128 || std::memcmp(d, "DDS ", 4) != 0) return fail(why, "not a DDS file");
    h = le32(d + 12), w = le32(d + 16);
    if (!w || !h || w > 8192 || h > 8192) return fail(why, "DDS size out of range");
    const std::uint32_t pf_flags = le32(d + 80), fourcc = le32(d + 84), bits = le32(d + 88);
    std::size_t at = 128;
    int kind = 0;  // 1 DXT1, 2 DXT3, 3 DXT5, 4 plain
    if (pf_flags & 4) {
        if (fourcc == 0x31545844u) kind = 1;       // "DXT1"
        else if (fourcc == 0x33545844u) kind = 2;  // "DXT3"
        else if (fourcc == 0x35545844u) kind = 3;  // "DXT5"
        else if (fourcc == 0x30315844u) {          // "DX10"
            if (size < 148) return fail(why, "short DDS");
            const std::uint32_t format = le32(d + 128);
            at = 148;
            if (format == 71 || format == 72) kind = 1;
            else if (format == 74 || format == 75) kind = 2;
            else if (format == 77 || format == 78) kind = 3;
            else if (format == 28 || format == 29) kind = 5;  // R8G8B8A8
            else return fail(why, "DDS format not supported (BC1/2/3 and RGBA8 are)");
        } else return fail(why, "DDS compression not supported (DXT1/3/5 are)");
    } else if ((pf_flags & 0x40) && (bits == 32 || bits == 24)) kind = 4;
    else return fail(why, "DDS pixel format not supported");
    out.assign(static_cast<std::size_t>(w) * h, 0);
    if (kind >= 1 && kind <= 3) {
        const std::size_t bw = (w + 3) / 4, bh = (h + 3) / 4, block = kind == 1 ? 8 : 16;
        if (size < at + bw * bh * block) return fail(why, "short DDS");
        Rgba px[16];
        for (std::size_t by = 0; by < bh; ++by)
            for (std::size_t bx = 0; bx < bw; ++bx) {
                const std::uint8_t *src = d + at + (by * bw + bx) * block;
                if (kind == 1) bc1_colors(src, false, px);
                else {
                    bc1_colors(src + 8, true, px);
                    if (kind == 2) bc2_alpha(src, px);
                    else bc3_alpha(src, px);
                }
                for (std::size_t n = 0; n < 16; ++n) {
                    const std::size_t x = bx * 4 + (n & 3), y = by * 4 + (n >> 2);
                    if (x < w && y < h) out[y * w + x] = rgba(px[n].r, px[n].g, px[n].b, px[n].a);
                }
            }
        return true;
    }
    if (kind == 5) {
        if (size < at + static_cast<std::size_t>(w) * h * 4) return fail(why, "short DDS");
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = le32(d + at + i * 4);
        return true;
    }
    const std::uint32_t rm = le32(d + 92), gm = le32(d + 96), bm = le32(d + 100), am = (pf_flags & 1) ? le32(d + 104) : 0;
    const std::size_t bpp = bits / 8;
    if (size < at + static_cast<std::size_t>(w) * h * bpp) return fail(why, "short DDS");
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint8_t *p = d + at + i * bpp;
        const std::uint32_t v = bpp == 4 ? le32(p) : (p[0] | p[1] << 8 | p[2] << 16);
        out[i] = rgba(mask_value(v, rm), mask_value(v, gm), mask_value(v, bm), am ? mask_value(v, am) : 255);
    }
    return true;
}

bool inflate_zlib(const std::uint8_t *data, std::size_t size, std::vector<std::uint8_t> &out) {
    if (size < 2 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20)) return false;
    Bits b{data + 2, size - 2};
    int last;
    do {
        last = b.need(1);
        const int type = b.need(2);
        if (b.bad) return false;
        if (type == 0) {
            b.buf = 0, b.cnt = 0;  // to the byte boundary
            if (b.pos + 4 > b.n) return false;
            const unsigned len = b.p[b.pos] | b.p[b.pos + 1] << 8, nlen = b.p[b.pos + 2] | b.p[b.pos + 3] << 8;
            b.pos += 4;
            if (len != (~nlen & 0xffffu) || b.pos + len > b.n) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
            b.pos += len;
        } else if (type == 1) {
            static Huffman lencode, distcode;
            static bool made = false;
            if (!made) {
                short lengths[288];
                int s = 0;
                for (; s < 144; ++s) lengths[s] = 8;
                for (; s < 256; ++s) lengths[s] = 9;
                for (; s < 280; ++s) lengths[s] = 7;
                for (; s < 288; ++s) lengths[s] = 8;
                build(lencode, lengths, 288);
                for (s = 0; s < 30; ++s) lengths[s] = 5;
                build(distcode, lengths, 30);
                made = true;
            }
            if (!codes(b, out, lencode, distcode)) return false;
        } else if (type == 2) {
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            const int nlen = b.need(5) + 257, ndist = b.need(5) + 1, ncode = b.need(4) + 4;
            if (b.bad || nlen > 286 || ndist > 30) return false;
            short lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(b.need(3));
            Huffman lencode, distcode;
            if (build(lencode, lengths, 19) != 0) return false;
            int index = 0;
            while (index < nlen + ndist) {
                int symbol = decode(b, lencode);
                if (symbol < 0) return false;
                if (symbol < 16) {
                    lengths[index++] = static_cast<short>(symbol);
                    continue;
                }
                short len = 0;
                if (symbol == 16) {
                    if (index == 0) return false;
                    len = lengths[index - 1];
                    symbol = 3 + b.need(2);
                } else if (symbol == 17) symbol = 3 + b.need(3);
                else symbol = 11 + b.need(7);
                if (index + symbol > nlen + ndist) return false;
                while (symbol--) lengths[index++] = len;
            }
            if (lengths[256] == 0) return false;
            const int lerr = build(lencode, lengths, nlen);
            if (lerr < 0 || (lerr > 0 && nlen - lencode.count[0] != 1)) return false;
            const int derr = build(distcode, lengths + nlen, ndist);
            if (derr < 0 || (derr > 0 && ndist - distcode.count[0] != 1)) return false;
            if (!codes(b, out, lencode, distcode)) return false;
        } else return false;
        if (b.bad) return false;
    } while (!last);
    return true;
}

bool decode_png(const std::uint8_t *d, std::size_t size, std::vector<std::uint32_t> &out, std::uint32_t &w, std::uint32_t &h, std::string *why) {
    static const std::uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < 8 || std::memcmp(d, sig, 8) != 0) return fail(why, "not a PNG file");
    std::size_t at = 8;
    int depth = 0, type = -1, interlace = 0;
    std::vector<std::uint8_t> idat;
    std::uint32_t palette[256];
    for (std::uint32_t &p : palette) p = 0xff000000u;
    w = h = 0;
    while (at + 8 <= size) {
        const std::uint32_t len = be32(d + at);
        const std::uint8_t *tag = d + at + 4, *body = d + at + 8;
        if (at + 12 + static_cast<std::size_t>(len) > size) return fail(why, "short PNG");
        if (!std::memcmp(tag, "IHDR", 4) && len >= 13) {
            w = be32(body), h = be32(body + 4);
            depth = body[8], type = body[9], interlace = body[12];
        } else if (!std::memcmp(tag, "PLTE", 4)) {
            for (std::uint32_t i = 0; i < len / 3 && i < 256; ++i) palette[i] = rgba(body[i * 3], body[i * 3 + 1], body[i * 3 + 2], 255);
        } else if (!std::memcmp(tag, "tRNS", 4) && type == 3) {
            for (std::uint32_t i = 0; i < len && i < 256; ++i) palette[i] = (palette[i] & 0x00ffffffu) | static_cast<std::uint32_t>(body[i]) << 24;
        } else if (!std::memcmp(tag, "IDAT", 4)) idat.insert(idat.end(), body, body + len);
        else if (!std::memcmp(tag, "IEND", 4)) break;
        at += 12 + static_cast<std::size_t>(len);
    }
    if (!w || !h || w > 8192 || h > 8192) return fail(why, "PNG size out of range");
    if (interlace) return fail(why, "interlaced PNG not supported");
    int channels = 0;
    switch (type) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return fail(why, "PNG colour type not supported");
    }
    if (!(depth == 8 || (depth == 16 && type != 3))) return fail(why, "PNG bit depth not supported (8 and 16 are)");
    const std::size_t bpp = static_cast<std::size_t>(channels) * (depth / 8), stride = bpp * w;
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * h);
    if (!inflate_zlib(idat.data(), idat.size(), raw) || raw.size() < (stride + 1) * h) return fail(why, "broken PNG data");
    std::vector<std::uint8_t> prev(stride, 0), cur(stride);
    out.assign(static_cast<std::size_t>(w) * h, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t filter = raw[y * (stride + 1)];
        const std::uint8_t *src = raw.data() + y * (stride + 1) + 1;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            int v = src[i];
            switch (filter) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            default: return fail(why, "broken PNG filter");
            }
            cur[i] = static_cast<std::uint8_t>(v);
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t *p = cur.data() + x * bpp;
            const std::size_t step = depth / 8;  // 16-bit: the high byte
            std::uint32_t px;
            switch (type) {
            case 0: px = rgba(p[0], p[0], p[0], 255); break;
            case 2: px = rgba(p[0], p[step], p[2 * step], 255); break;
            case 3: px = palette[p[0]]; break;
            case 4: px = rgba(p[0], p[0], p[0], p[step]); break;
            default: px = rgba(p[0], p[step], p[2 * step], p[3 * step]); break;
            }
            out[static_cast<std::size_t>(y) * w + x] = px;
        }
        std::swap(prev, cur);
    }
    return true;
}

bool load_picture(const std::string &path, std::vector<std::uint32_t> &out, std::uint32_t &w, std::uint32_t &h, std::string *why) {
    std::vector<std::uint8_t> bytes;
    if (!read_whole(path, bytes)) return fail(why, "cannot read the file");
    const std::string ext = path.size() >= 4 ? lower(path.substr(path.size() - 4)) : "";
    if (ext == ".png") return decode_png(bytes.data(), bytes.size(), out, w, h, why);
    return decode_dds(bytes.data(), bytes.size(), out, w, h, why);
}

void shrink(const std::uint32_t *src, std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh, std::vector<std::uint32_t> &out) {
    // RGBA -> RGBa (premultiplied, Pillow's rgbA2rgba)
    std::vector<std::uint8_t> pre(static_cast<std::size_t>(sw) * sh * 4);
    for (std::size_t i = 0, n = static_cast<std::size_t>(sw) * sh; i < n; ++i) {
        const std::uint32_t p = src[i];
        const int a = static_cast<int>(p >> 24);
        pre[i * 4] = muldiv255(static_cast<int>(p & 0xff), a);
        pre[i * 4 + 1] = muldiv255(static_cast<int>((p >> 8) & 0xff), a);
        pre[i * 4 + 2] = muldiv255(static_cast<int>((p >> 16) & 0xff), a);
        pre[i * 4 + 3] = static_cast<std::uint8_t>(a);
    }
    // rows (Pillow: horizontal first), then columns
    std::vector<std::uint8_t> mid(static_cast<std::size_t>(dw) * sh * 4);
    if (dw != sw) {
        const Coeffs c = box_coeffs(static_cast<int>(sw), static_cast<int>(dw));
        for (std::uint32_t y = 0; y < sh; ++y)
            for (std::uint32_t x = 0; x < dw; ++x) {
                const std::int32_t *k = c.k.data() + static_cast<std::size_t>(x) * c.ksize;
                for (int ch = 0; ch < 4; ++ch) {
                    std::int64_t ss = 1 << (kPrecision - 1);
                    for (int i = 0; i < c.size[x]; ++i) ss += static_cast<std::int64_t>(pre[(static_cast<std::size_t>(y) * sw + c.start[x] + i) * 4 + ch]) * k[i];
                    mid[(static_cast<std::size_t>(y) * dw + x) * 4 + ch] = clip8(ss);
                }
            }
    } else mid = pre;
    std::vector<std::uint8_t> fin(static_cast<std::size_t>(dw) * dh * 4);
    if (dh != sh) {
        const Coeffs c = box_coeffs(static_cast<int>(sh), static_cast<int>(dh));
        for (std::uint32_t y = 0; y < dh; ++y) {
            const std::int32_t *k = c.k.data() + static_cast<std::size_t>(y) * c.ksize;
            for (std::uint32_t x = 0; x < dw; ++x)
                for (int ch = 0; ch < 4; ++ch) {
                    std::int64_t ss = 1 << (kPrecision - 1);
                    for (int i = 0; i < c.size[y]; ++i) ss += static_cast<std::int64_t>(mid[(static_cast<std::size_t>(c.start[y] + i) * dw + x) * 4 + ch]) * k[i];
                    fin[(static_cast<std::size_t>(y) * dw + x) * 4 + ch] = clip8(ss);
                }
        }
    } else fin = mid;
    // RGBa -> RGBA (Pillow's rgba2rgbA)
    out.resize(static_cast<std::size_t>(dw) * dh);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint8_t *p = fin.data() + i * 4;
        const unsigned a = p[3];
        std::uint32_t r = p[0], g = p[1], b = p[2];
        if (a != 255 && a != 0) {
            r = std::min(255u, 255u * r / a), g = std::min(255u, 255u * g / a), b = std::min(255u, 255u * b / a);
        }
        out[i] = rgba(r, g, b, a);
    }
}

double difference(const std::uint32_t *a, const std::uint32_t *b, std::size_t count) {
    std::uint64_t num = 0, den = 0, da = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t p = a[i], q = b[i];
        const int ar = p & 0xff, ag = (p >> 8) & 0xff, ab = (p >> 16) & 0xff, aa = static_cast<int>(p >> 24);
        const int br = q & 0xff, bg = (q >> 8) & 0xff, bb = (q >> 16) & 0xff, ba = static_cast<int>(q >> 24);
        const int wi = std::min(128, std::max(aa, ba));
        num += static_cast<std::uint64_t>(wi) * static_cast<std::uint64_t>(std::abs(ar - br) + std::abs(ag - bg) + std::abs(ab - bb));
        den += static_cast<std::uint64_t>(wi);
        da += static_cast<std::uint64_t>(std::abs(aa - ba));
    }
    // tex_match.py: sum(mean|rgb| * w) / max(sum(w), 1) + 0.5 * mean|a|, with w = min(1, max(alpha) / 128)
    const double d_rgb = static_cast<double>(num) / 3.0 / 128.0 / std::max(static_cast<double>(den) / 128.0, 1.0);
    return d_rgb + 0.5 * static_cast<double>(da) / static_cast<double>(count ? count : 1);
}

bool is_plain(const std::uint32_t *pixels, std::size_t count) {
    for (std::size_t i = 1; i < count; ++i)
        if (pixels[i] != pixels[0]) return false;
    return true;
}

// ---- Pack

Pack::Pack(std::string folder, std::string cache) : folder_(std::move(folder)), cache_dir_(std::move(cache)) {}

void Pack::scan() {
    files_.clear();
    by_name_.clear();
    std::error_code error;
    const std::filesystem::path root = os::path(folder_);
    for (auto it = std::filesystem::recursive_directory_iterator(root, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        Name n;
        if (!parse_name(os::utf8(it->path().filename()), n)) continue;
        File f;
        f.path = os::utf8(it->path());
        f.name = os::utf8(it->path().lexically_relative(root));
        std::replace(f.name.begin(), f.name.end(), '\\', '/');
        f.size = it->file_size(error);
        f.time = static_cast<std::int64_t>(it->last_write_time(error).time_since_epoch().count());
        f.tw = n.tw, f.th = n.th;
        files_.push_back(std::move(f));
    }
    std::sort(files_.begin(), files_.end(), [](const File &x, const File &y) { return x.name < y.name; });
    for (std::size_t i = 0; i < files_.size(); ++i) by_name_[files_[i].name] = static_cast<int>(i);
    // the match cache: "<our hash hex> <file name or ->" per line
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.clear();
    if (cache_dir().empty()) return;
    if (std::FILE *f = os::open(cache_dir() + "/matches.txt", "rb")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string text = line;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            const std::size_t space = text.find(' ');
            if (space != 16) continue;
            const std::uint64_t ours = std::strtoull(text.substr(0, 16).c_str(), nullptr, 16);
            const std::string file = text.substr(space + 1);
            if (file == "-") cache_[ours] = -1;
            else if (const auto it = by_name_.find(file); it != by_name_.end()) cache_[ours] = it->second;
            // (a file no longer in the pack: looked at again)
        }
        std::fclose(f);
    }
}

namespace {
constexpr char kThumbMagic[12] = {'P', 'S', '2', 'T', 'H', 'U', 'M', 'B', 'S', '0', '1', 0};
}

bool Pack::read_thumbs() {
    if (cache_dir().empty()) return false;
    std::vector<std::uint8_t> bytes;
    if (!read_whole(cache_dir() + "/thumbs.bin", bytes) || bytes.size() < 16 || std::memcmp(bytes.data(), kThumbMagic, 12) != 0) return false;
    std::size_t at = 12;
    const auto u32 = [&](std::uint32_t &v) {
        if (at + 4 > bytes.size()) return false;
        v = le32(bytes.data() + at);
        at += 4;
        return true;
    };
    const auto u64 = [&](std::uint64_t &v) {
        std::uint32_t lo = 0, hi = 0;
        if (!u32(lo) || !u32(hi)) return false;
        v = lo | static_cast<std::uint64_t>(hi) << 32;
        return true;
    };
    std::uint32_t count = 0;
    if (!u32(count) || count != files_.size()) return false;
    for (const File &f : files_) {
        std::uint32_t len = 0;
        std::uint64_t size = 0, time = 0;
        if (!u32(len) || at + len > bytes.size()) return false;
        const std::string name(reinterpret_cast<const char *>(bytes.data() + at), len);
        at += len;
        if (!u64(size) || !u64(time) || name != f.name || size != f.size || static_cast<std::int64_t>(time) != f.time) return false;
    }
    std::unordered_map<int, Bucket> buckets;
    for (std::size_t i = 0; i < files_.size(); ++i) {
        const File &f = files_[i];
        const std::size_t n = static_cast<std::size_t>(1u << f.tw) * (1u << f.th);
        if (at + n * 4 > bytes.size()) return false;
        Bucket &b = buckets[f.tw | f.th << 4];
        b.files.push_back(static_cast<int>(i));
        const std::size_t old = b.pixels.size();
        b.pixels.resize(old + n);
        std::memcpy(b.pixels.data() + old, bytes.data() + at, n * 4);
        at += n * 4;
    }
    buckets_ = std::move(buckets);
    return true;
}

void Pack::write_thumbs() const {
    if (cache_dir().empty()) return;
    std::error_code error;
    std::filesystem::create_directories(os::path(cache_dir()), error);
    std::vector<std::uint8_t> bytes(kThumbMagic, kThumbMagic + 12);
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    u32(static_cast<std::uint32_t>(files_.size()));
    for (const File &f : files_) {
        u32(static_cast<std::uint32_t>(f.name.size()));
        bytes.insert(bytes.end(), f.name.begin(), f.name.end());
        u32(static_cast<std::uint32_t>(f.size)), u32(static_cast<std::uint32_t>(f.size >> 32));
        u32(static_cast<std::uint32_t>(f.time)), u32(static_cast<std::uint32_t>(static_cast<std::uint64_t>(f.time) >> 32));
    }
    // each file's thumbnail in file order (a bucket holds its files in that order too)
    std::unordered_map<int, std::size_t> next;
    for (const File &f : files_) {
        const int key = f.tw | f.th << 4;
        const Bucket &b = buckets_.at(key);
        const std::size_t n = static_cast<std::size_t>(1u << f.tw) * (1u << f.th);
        const std::size_t at = next[key]++ * n;
        const auto *p = reinterpret_cast<const std::uint8_t *>(b.pixels.data() + at);
        bytes.insert(bytes.end(), p, p + n * 4);
    }
    os::replace_file(cache_dir() + "/thumbs.bin", bytes.data(), bytes.size());
}

void Pack::build_index(const std::function<bool()> &stop) {
    const auto start = std::chrono::steady_clock::now();
    ready_ = false;
    index_from_file = read_thumbs();
    if (!index_from_file) {
        std::unordered_map<int, Bucket> buckets;
        std::vector<std::uint32_t> big, small;
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (stop && stop()) return;
            const File &f = files_[i];
            const std::uint32_t nw = 1u << f.tw, nh = 1u << f.th;
            std::uint32_t bw = 0, bh = 0;
            std::string why;
            if (!load_picture(f.path, big, bw, bh, &why)) {
                std::fprintf(stderr, "[textures] %s: %s (left out)\n", f.path.c_str(), why.c_str());
                big.assign(static_cast<std::size_t>(nw) * nh, 0u);  // a thumbnail that matches nothing visible
                bw = nw, bh = nh;
            }
            shrink(big.data(), bw, bh, nw, nh, small);
            Bucket &b = buckets[f.tw | f.th << 4];
            b.files.push_back(static_cast<int>(i));
            b.pixels.insert(b.pixels.end(), small.begin(), small.end());
        }
        buckets_ = std::move(buckets);
        write_thumbs();
    }
    index_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    ready_ = true;
}

Pack::Result Pack::match(const std::uint32_t *pixels, std::uint32_t w, std::uint32_t h, double threshold) const {
    Result best;
    int tw = 0, th = 0;
    while ((1u << tw) < w) ++tw;
    while ((1u << th) < h) ++th;
    if ((1u << tw) != w || (1u << th) != h || is_plain(pixels, static_cast<std::size_t>(w) * h)) return best;
    const auto it = buckets_.find(tw | th << 4);
    if (it == buckets_.end()) return best;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    for (std::size_t i = 0; i < it->second.files.size(); ++i) {
        const double d = difference(pixels, it->second.pixels.data() + i * n, n);
        if (d < best.diff) best.diff = d, best.file = it->second.files[i];
    }
    if (best.diff >= threshold) best.file = -1;
    return best;
}

int Pack::cached(std::uint64_t ours, bool &found) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const auto it = cache_.find(ours);
    found = it != cache_.end();
    return found ? it->second : -1;
}

void Pack::remember(std::uint64_t ours, int file) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[ours] = file;
    }
    if (cache_dir().empty()) return;
    std::error_code error;
    std::filesystem::create_directories(os::path(cache_dir()), error);
    if (std::FILE *f = os::open(cache_dir() + "/matches.txt", "ab")) {
        std::fprintf(f, "%016llx %s\n", static_cast<unsigned long long>(ours), file >= 0 ? files_[static_cast<std::size_t>(file)].name.c_str() : "-");
        std::fclose(f);
    }
}

std::size_t Pack::cached_count() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return cache_.size();
}

}  // namespace ps2::texpack
