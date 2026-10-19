#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace title {

// Source font layout in authored glyph units. Text is a byte string, '~'
// starts a line, and each glyph's page lives in its x float's low two bits.
class TextLayout {
public:
    struct Glyph { float u = 0, v = 0, width = 0, height = 0; };
    struct Font {
        int32_t first_character = 0;
        float width = 1, height = 1;
        std::vector<Glyph> glyphs;
    };
    struct Item {
        float x = 0, y = 0, scale_x = 1, scale_y = 1;
        uint32_t colour = 0x80808080, alignment = 0;
        std::string text;
    };
    struct Line {
        float x = 0, y = 0, width = 0, height = 0;
        // Includes the source's four-glyph padding. Glyph zero can advance
        // the cursor or draw; do not assume it is an invisible space.
        std::vector<uint8_t> glyphs;
    };
    struct Group { size_t first = 0, count = 0; };
    struct Output {
        float scale_x = 0, scale_y = 0, height = 0;
        uint32_t colour = 0;
        std::vector<Line> lines;
        // Source batching limits affect draw/alpha ordering.
        std::vector<Group> groups;
        std::array<bool, 4> pages{};
    };
    enum class Result { Ok, InvalidFont, InvalidItem, LimitExceeded };
    static constexpr size_t kMaxGlyphs = 0x800, kMaxLines = 64;
    static uint8_t Page(const Glyph& glyph);
    // Exact finite source arithmetic, isolated from caller rounding mode.
    // Declines source buffer overflow/nonprogressing line groups explicitly.
    static Result Build(const Font& font, int32_t frame_width, int32_t frame_height,
                        const Item& item, Output& out);

private:
    struct Rounding;
};

}  // namespace title
