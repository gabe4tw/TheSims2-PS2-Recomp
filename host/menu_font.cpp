#include "menu_font.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace menufont {
namespace {

// The 5 x 8 letters (moved from quickmenu.cpp): printable ASCII (32 to 126), drawn for this project (no font file, no licence to
// carry). One letter a line, its 8 rows from the top separated by spaces, '#' a lit pixel. Row 7 is for the tails of g, j, p, q, y.
constexpr const char *kFont[95] = {
    "..... ..... ..... ..... ..... ..... ..... .....",  // space
    "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#.. .....",  // !
    ".#.#. .#.#. ..... ..... ..... ..... ..... .....",  // "
    ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#. .....",  // #
    "..#.. .#### #.#.. .###. ..#.# ####. ..#.. .....",  // $
    "##... ##..# ...#. ..#.. .#... #..## ...## .....",  // %
    ".##.. #..#. #.#.. .#... #.#.# #..#. .##.# .....",  // &
    "..#.. ..#.. ..... ..... ..... ..... ..... .....",  // '
    "...#. ..#.. .#... .#... .#... ..#.. ...#. .....",  // (
    ".#... ..#.. ...#. ...#. ...#. ..#.. .#... .....",  // )
    "..... ..#.. #.#.# .###. #.#.# ..#.. ..... .....",  // *
    "..... ..#.. ..#.. ##### ..#.. ..#.. ..... .....",  // +
    "..... ..... ..... ..... ..... .##.. ..#.. .#...",  // ,
    "..... ..... ..... ##### ..... ..... ..... .....",  // -
    "..... ..... ..... ..... ..... .##.. .##.. .....",  // .
    "..... ....# ...#. ..#.. .#... #.... ..... .....",  // /
    ".###. #...# #..## #.#.# ##..# #...# .###. .....",  // 0
    "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // 1
    ".###. #...# ....# ...#. ..#.. .#... ##### .....",  // 2
    "##### ...#. ..#.. ...#. ....# #...# .###. .....",  // 3
    "...#. ..##. .#.#. #..#. ##### ...#. ...#. .....",  // 4
    "##### #.... ####. ....# ....# #...# .###. .....",  // 5
    "..##. .#... #.... ####. #...# #...# .###. .....",  // 6
    "##### ....# ...#. ..#.. .#... .#... .#... .....",  // 7
    ".###. #...# #...# .###. #...# #...# .###. .....",  // 8
    ".###. #...# #...# .#### ....# ...#. .##.. .....",  // 9
    "..... .##.. .##.. ..... .##.. .##.. ..... .....",  // :
    "..... .##.. .##.. ..... .##.. ..#.. .#... .....",  // ;
    "...#. ..#.. .#... #.... .#... ..#.. ...#. .....",  // <
    "..... ..... ##### ..... ##### ..... ..... .....",  // =
    ".#... ..#.. ...#. ....# ...#. ..#.. .#... .....",  // >
    ".###. #...# ....# ...#. ..#.. ..... ..#.. .....",  // ?
    ".###. #...# ....# .##.# #.#.# #.#.# .###. .....",  // @
    ".###. #...# #...# ##### #...# #...# #...# .....",  // A
    "####. #...# #...# ####. #...# #...# ####. .....",  // B
    ".###. #...# #.... #.... #.... #...# .###. .....",  // C
    "####. #...# #...# #...# #...# #...# ####. .....",  // D
    "##### #.... #.... ####. #.... #.... ##### .....",  // E
    "##### #.... #.... ####. #.... #.... #.... .....",  // F
    ".###. #...# #.... #.### #...# #...# .#### .....",  // G
    "#...# #...# #...# ##### #...# #...# #...# .....",  // H
    ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // I
    "..### ...#. ...#. ...#. ...#. #..#. .##.. .....",  // J
    "#...# #..#. #.#.. ##... #.#.. #..#. #...# .....",  // K
    "#.... #.... #.... #.... #.... #.... ##### .....",  // L
    "#...# ##.## #.#.# #.#.# #...# #...# #...# .....",  // M
    "#...# #...# ##..# #.#.# #..## #...# #...# .....",  // N
    ".###. #...# #...# #...# #...# #...# .###. .....",  // O
    "####. #...# #...# ####. #.... #.... #.... .....",  // P
    ".###. #...# #...# #...# #.#.# #..#. .##.# .....",  // Q
    "####. #...# #...# ####. #.#.. #..#. #...# .....",  // R
    ".#### #.... #.... .###. ....# ....# ####. .....",  // S
    "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",  // T
    "#...# #...# #...# #...# #...# #...# .###. .....",  // U
    "#...# #...# #...# #...# #...# .#.#. ..#.. .....",  // V
    "#...# #...# #...# #.#.# #.#.# #.#.# .#.#. .....",  // W
    "#...# #...# .#.#. ..#.. .#.#. #...# #...# .....",  // X
    "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#.. .....",  // Y
    "##### ....# ...#. ..#.. .#... #.... ##### .....",  // Z
    ".###. .#... .#... .#... .#... .#... .###. .....",  // [
    "..... #.... .#... ..#.. ...#. ....# ..... .....",  // backslash
    ".###. ...#. ...#. ...#. ...#. ...#. .###. .....",  // ]
    "..#.. .#.#. #...# ..... ..... ..... ..... .....",  // ^
    "..... ..... ..... ..... ..... ..... ##### .....",  // _
    ".#... ..#.. ..... ..... ..... ..... ..... .....",  // `
    "..... ..... .###. ....# .#### #...# .#### .....",  // a
    "#.... #.... #.##. ##..# #...# #...# ####. .....",  // b
    "..... ..... .###. #.... #.... #...# .###. .....",  // c
    "....# ....# .##.# #..## #...# #...# .#### .....",  // d
    "..... ..... .###. #...# ##### #.... .###. .....",  // e
    "..##. .#..# .#... ###.. .#... .#... .#... .....",  // f
    "..... ..... .#### #...# #...# .#### ....# .###.",  // g
    "#.... #.... #.##. ##..# #...# #...# #...# .....",  // h
    "..#.. ..... .##.. ..#.. ..#.. ..#.. .###. .....",  // i
    "...#. ..... ..##. ...#. ...#. ...#. #..#. .##..",  // j
    "#.... #.... #..#. #.#.. ##... #.#.. #..#. .....",  // k
    ".##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // l
    "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.# .....",  // m
    "..... ..... #.##. ##..# #...# #...# #...# .....",  // n
    "..... ..... .###. #...# #...# #...# .###. .....",  // o
    "..... ..... ####. #...# #...# ####. #.... #....",  // p
    "..... ..... .#### #...# #...# .#### ....# ....#",  // q
    "..... ..... #.##. ##..# #.... #.... #.... .....",  // r
    "..... ..... .###. #.... .###. ....# ####. .....",  // s
    ".#... .#... ###.. .#... .#... .#..# ..##. .....",  // t
    "..... ..... #...# #...# #...# #..## .##.# .....",  // u
    "..... ..... #...# #...# #...# .#.#. ..#.. .....",  // v
    "..... ..... #...# #...# #.#.# #.#.# .#.#. .....",  // w
    "..... ..... #...# .#.#. ..#.. .#.#. #...# .....",  // x
    "..... ..... #...# #...# #...# .#### ....# .###.",  // y
    "..... ..... ##### ...#. ..#.. .#... ##### .....",  // z
    "...#. ..#.. ..#.. .#... ..#.. ..#.. ...#. .....",  // {
    "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",  // |
    ".#... ..#.. ..#.. ...#. ..#.. ..#.. .#... .....",  // }
    "..... ..... .#... #.#.# ...#. ..... ..... .....",  // ~
};

int fallback_scale(float px) { return std::max(1, static_cast<int>(std::lround(px / 10.0f))); }

// the next code point of UTF-8 text (a broken sequence counts as one '?')
std::uint32_t next_code(const std::string &s, std::size_t &i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = byte(i);
    const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (extra == 0) {
        ++i;
        return c;
    }
    if (extra < 0 || i + extra >= s.size()) {
        ++i;
        return '?';
    }
    std::uint32_t code = c & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k) {
        if ((byte(i + k) & 0xC0) != 0x80) {
            ++i;
            return '?';
        }
        code = code << 6 | (byte(i + k) & 0x3F);
    }
    i += extra + 1;
    return code;
}

std::uint32_t be32(const std::vector<unsigned char> &d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) << 24 | static_cast<std::uint32_t>(d[at + 1]) << 16 | static_cast<std::uint32_t>(d[at + 2]) << 8 | d[at + 3];
}

// stb_truetype checks nothing: only a file whose table list and tables lie inside it is handed over
bool plausible_font(const std::vector<unsigned char> &d) {
    if (d.size() < 12) return false;
    const std::uint32_t tag = be32(d, 0);
    if (tag != 0x00010000u && tag != 0x74727565u /* true */ && tag != 0x4F54544Fu /* OTTO */) return false;
    const std::size_t tables = static_cast<std::size_t>(d[4]) << 8 | d[5];
    if (tables == 0 || 12 + 16 * tables > d.size()) return false;
    // the tables stb_truetype reads fields from, and the least length that holds the fields it reads (head: indexToLocFormat at 50,
    // hhea: numberOfHMetrics at 34, maxp: numGlyphs at 4); a file without them, or with them cut short, is not handed over
    struct Need {
        std::uint32_t tag;
        std::size_t least;
        bool found;
    } need[] = {{0x68656164u /* head */, 54, false}, {0x68686561u /* hhea */, 36, false}, {0x6D617870u /* maxp */, 6, false},
                {0x636D6170u /* cmap */, 4, false}, {0x686D7478u /* hmtx */, 4, false}};
    for (std::size_t t = 0; t < tables; ++t) {
        const std::size_t at = 12 + 16 * t, offset = be32(d, at + 8), length = be32(d, at + 12);
        if (offset > d.size() || length > d.size() - offset) return false;
        for (Need &n : need)
            if (be32(d, at) == n.tag) {
                if (length < n.least) return false;
                n.found = true;
            }
    }
    for (const Need &n : need)
        if (!n.found) return false;
    return true;
}

}  // namespace

struct Font::Impl {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
    int ascent = 0, descent = 0, gap = 0;
    struct Glyph {
        int w = 0, h = 0, x = 0, y = 0;
        std::vector<unsigned char> alpha;
    };
    mutable std::map<std::pair<std::uint32_t, int>, Glyph> glyphs;  // (code point, size in quarter pixels)
    float scale(float px) const { return stbtt_ScaleForPixelHeight(&info, px); }
    const Glyph &glyph(std::uint32_t code, float px) const {
        const std::pair<std::uint32_t, int> key{code, static_cast<int>(std::lround(px * 4))};
        auto it = glyphs.find(key);
        if (it != glyphs.end()) return it->second;
        // every window height makes its own sizes (dragging the window's edge makes hundreds): past this many the old ones go.
        // (No caller keeps a glyph across calls, so clearing here leaves nothing dangling.)
        if (glyphs.size() >= 4096) glyphs.clear();
        Glyph g;
        const float s = scale(px);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&info, static_cast<int>(code), s, s, &x0, &y0, &x1, &y1);
        g.w = std::max(0, x1 - x0);
        g.h = std::max(0, y1 - y0);
        g.x = x0;
        g.y = y0;
        g.alpha.assign(static_cast<std::size_t>(g.w) * g.h, 0);
        if (g.w && g.h) stbtt_MakeCodepointBitmap(&info, g.alpha.data(), g.w, g.h, g.w, s, s, static_cast<int>(code));
        return glyphs.emplace(key, std::move(g)).first->second;
    }
};

Font::Font() : impl_(std::make_unique<Impl>()) {}
Font::~Font() = default;

bool Font::load(const std::string &ttf_path) {
    impl_ = std::make_unique<Impl>();
    std::FILE *in = ps2::os::open(ttf_path, "rb");  // UTF-8 path (a folder name outside the Windows code page works)
    if (!in) return false;
    std::vector<unsigned char> data;
    unsigned char buffer[65536];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, in)) > 0;) data.insert(data.end(), buffer, buffer + n);
    std::fclose(in);
    if (!plausible_font(data)) return false;
    impl_->data = std::move(data);
    if (!stbtt_InitFont(&impl_->info, impl_->data.data(), stbtt_GetFontOffsetForIndex(impl_->data.data(), 0))) {
        impl_ = std::make_unique<Impl>();
        return false;
    }
    stbtt_GetFontVMetrics(&impl_->info, &impl_->ascent, &impl_->descent, &impl_->gap);
    impl_->ok = true;
    return true;
}

bool Font::is_truetype() const { return impl_->ok; }

float Font::line_height(float px) const {
    if (!impl_->ok) return static_cast<float>(10 * fallback_scale(px));
    return std::ceil((impl_->ascent - impl_->descent + impl_->gap) * impl_->scale(px));
}

float Font::width(const std::string &utf8, float px) const {
    if (!impl_->ok) {
        float n = 0;
        for (std::size_t i = 0; i < utf8.size(); ++n) next_code(utf8, i);
        return n * 6 * fallback_scale(px);
    }
    const float s = impl_->scale(px);
    float w = 0;
    std::uint32_t last = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        const std::uint32_t code = next_code(utf8, i);
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&impl_->info, static_cast<int>(code), &advance, &bearing);
        if (last) w += stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(last), static_cast<int>(code)) * s;
        w += advance * s;
        last = code;
    }
    return w;
}

void Font::draw(menudraw::Canvas &c, float x, float top, float px, const std::string &utf8, std::uint32_t color) const {
    if (!impl_->ok) {
        const int s = fallback_scale(px);
        const int ox = static_cast<int>(std::lround(x)), oy = static_cast<int>(std::lround(top)) + s;
        int pen = 0;
        for (std::size_t i = 0; i < utf8.size();) {
            const std::uint32_t code = next_code(utf8, i);
            const char *g = kFont[(code >= 32 && code <= 126 ? code : '?') - 32];
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 5; ++col)
                    if (g[row * 6 + col] == '#')
                        c.fill_rect(static_cast<float>(ox + pen + col * s), static_cast<float>(oy + row * s), static_cast<float>(s), static_cast<float>(s), color);
            pen += 6 * s;
        }
        return;
    }
    const float s = impl_->scale(px);
    const float baseline = std::round(top + impl_->ascent * s);
    float pen = x;
    std::uint32_t last = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        const std::uint32_t code = next_code(utf8, i);
        if (last) pen += stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(last), static_cast<int>(code)) * s;
        const Impl::Glyph &g = impl_->glyph(code, px);
        const int gx = static_cast<int>(std::lround(pen)) + g.x, gy = static_cast<int>(baseline) + g.y;
        for (int row = 0; row < g.h; ++row)
            for (int col = 0; col < g.w; ++col)
                if (const unsigned char a = g.alpha[static_cast<std::size_t>(row) * g.w + col]) c.blend(gx + col, gy + row, color, a / 255.0f);
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&impl_->info, static_cast<int>(code), &advance, &bearing);
        pen += advance * s;
        last = code;
    }
}

std::string fit_text(const Font &f, const std::string &utf8, float px, float max_width) {
    if (f.width(utf8, px) <= max_width) return utf8;
    if (f.width("...", px) > max_width) return "";
    std::vector<std::size_t> ends;  // where each code point ends
    for (std::size_t i = 0; i < utf8.size();) {
        next_code(utf8, i);
        ends.push_back(i);
    }
    for (std::size_t n = ends.size(); n-- > 0;) {
        std::string cut = utf8.substr(0, n ? ends[n - 1] : 0);
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        cut += "...";
        if (f.width(cut, px) <= max_width) return cut;
    }
    return "...";
}

}  // namespace menufont
