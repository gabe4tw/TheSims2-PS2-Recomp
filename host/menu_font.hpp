#pragma once

// Local change: the quick menu's text. A TrueType font file (games/<game>/menu/menu.ttf, given by PS2_MENU_FONT) drawn with
// stb_truetype at the window's own size, with soft edges; when there is no usable font file, the menu's old 5 x 8 pixel letters
// (printable ASCII, drawn for this project) so the menu always works.

#include "menu_draw.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace menufont {

class Font {
public:
    Font();
    ~Font();
    Font(const Font &) = delete;
    Font &operator=(const Font &) = delete;
    // false (and the 5 x 8 letters stay) when the file is missing or is not a font
    bool load(const std::string &ttf_path);
    bool is_truetype() const;
    float line_height(float px) const;
    float width(const std::string &utf8, float px) const;
    // `top` is the top of the line (line_height(px) tall)
    void draw(menudraw::Canvas &c, float x, float top, float px, const std::string &utf8, std::uint32_t color) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The text, or the longest start of it followed by "...", that is at most `max_width` wide ("" when not even "..." fits).
std::string fit_text(const Font &f, const std::string &utf8, float px, float max_width);

}  // namespace menufont
