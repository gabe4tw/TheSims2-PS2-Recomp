// The quick menu's text (host/menu_font.hpp), no game and no window needed:
// ninja ps2menufonttest && ./ps2menufonttest <font.ttf>   (default games/sims2/menu/menu.ttf, from the repo root)
#include "../host/menu_font.hpp"
#include "ps2/os.hpp"

#include <filesystem>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failed = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

menudraw::Picture blank(int w, int h) {
    menudraw::Picture p;
    p.width = w;
    p.height = h;
    p.rgba.assign(static_cast<std::size_t>(w) * h, 0);
    return p;
}

int lit(const menudraw::Picture &p, bool *partial = nullptr) {
    int n = 0;
    for (std::uint32_t v : p.rgba) {
        const int a = static_cast<int>(v >> 24);
        if (a) ++n;
        if (partial && a > 0 && a < 255) *partial = true;
    }
    return n;
}

}  // namespace

int main(int argc, char **argv) {
    using menufont::Font;
    const std::string path = argc > 1 ? argv[1] : "games/sims2/menu/menu.ttf";
    {
        Font f;
        check(f.width("abc", 20) == 36 && f.line_height(20) == 20, "fallback_metrics");
    }
    {
        Font f;
        const bool loaded = f.load("no/such.ttf");
        check(!loaded && !f.is_truetype() && f.width("abc", 20) == 36, "missing_file_falls_back");
    }
    {
        const std::string junk = "menu_font_test_junk.ttf";
        if (std::FILE *out = std::fopen(junk.c_str(), "wb")) {
            std::srand(7);
            for (int i = 0; i < 64; ++i) std::fputc(std::rand() & 255, out);
            std::fclose(out);
        }
        Font f;
        const bool loaded = f.load(junk);
        menudraw::Picture p = blank(200, 50);
        menudraw::Canvas c(p);
        f.draw(c, 2, 2, 20, "Hello", menudraw::hex(0x1E2A78));
        std::remove(junk.c_str());
        check(!loaded && lit(p) > 0, "font_garbage_file_falls_back");
    }
    Font tt;
    const bool tt_ok = tt.load(path);
    {
        menudraw::Picture p = blank(200, 50);
        menudraw::Canvas c(p);
        bool partial = false;
        if (tt_ok) tt.draw(c, 2, 2, 24, "Graphics", menudraw::hex(0x1E2A78));
        const float w = tt.width("Graphics", 24);
        check(tt_ok && tt.is_truetype() && w >= 60 && w <= 130 && lit(p, &partial) > 0 && partial, "truetype_loads");
    }
    check(tt_ok && tt.width("\xC3\x84", 24) > 0, "utf8_umlaut");
    {
        const std::string text = "first two are players 1 and 2";
        bool ok = tt_ok;
        for (float max : {10.0f, 40.0f, 80.0f, 300.0f}) {
            const std::string cut = menufont::fit_text(tt, text, 20, max);
            ok = ok && tt.width(cut, 20) <= max;
            if (max == 300.0f) ok = ok && cut == text;
            if (max == 40.0f) ok = ok && cut.size() >= 3 && cut.compare(cut.size() - 3, 3, "...") == 0;
        }
        check(ok, "fit_text_never_exceeds_width");
    }
    {
        // a font file whose path has letters outside the Windows code page (a Cyrillic user folder, say) still loads
        const std::string copy = "menu_font_\xD1\x82\xD0\xB5\xD1\x81\xD1\x82.ttf";  // "тест", UTF-8
        bool written = false;
        if (std::FILE *in = ps2::os::open(path, "rb")) {
            if (std::FILE *out = ps2::os::open(copy, "wb")) {
                char buffer[65536];
                for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, in)) > 0;) std::fwrite(buffer, 1, n, out);
                std::fclose(out);
                written = true;
            }
            std::fclose(in);
        }
        Font f;
        const bool loaded = written && f.load(copy);
        std::filesystem::remove(ps2::os::path(copy));
        check(loaded && f.is_truetype(), "unicode_path_loads");
    }
    {
        // a hand-made file whose table list is fine but whose 'head' table is far too short is refused (stb would read past it)
        const std::string bad = "menu_font_test_short.ttf";
        if (std::FILE *out = std::fopen(bad.c_str(), "wb")) {
            const unsigned char dir[] = {0, 1, 0, 0, 0, 1, 0, 16, 0, 0, 0, 0,  // sfnt 1.0, one table
                                         'h', 'e', 'a', 'd', 0, 0, 0, 0, 0, 0, 0, 28, 0, 0, 0, 4};
            std::fwrite(dir, 1, sizeof dir, out);
            for (int i = 0; i < 4; ++i) std::fputc(0, out);
            std::fclose(out);
        }
        Font f;
        const bool loaded = f.load(bad);
        std::remove(bad.c_str());
        check(!loaded && !f.is_truetype(), "short_tables_fall_back");
    }
    {
        // many window heights (a drag-resize) keep working past the cache's limit
        bool ok = tt_ok;
        menudraw::Picture p = blank(100, 40);
        menudraw::Canvas c(p);
        for (int h = 0; h < 800 && ok; ++h) {
            tt.draw(c, 2, 2, 10.0f + h * 0.25f, "Graphics", menudraw::hex(0x1E2A78));
            ok = tt.width("Graphics", 10.0f + h * 0.25f) > 0;
        }
        check(ok, "glyph_cache_survives_many_sizes");
    }
    std::printf(g_failed ? "%d FAILED\n" : "all ok\n", g_failed);
    return g_failed ? 1 : 0;
}
