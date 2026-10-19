#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_layout.h"

#include <cfenv>
#include <cmath>
#include <cstring>
#include <utility>

#include "engine/ee/cpu/ps2_float.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {
struct TextLayout::Rounding {
    int previous = std::fegetround();
    Rounding() { std::fesetround(FE_TOWARDZERO); }
    ~Rounding() { std::fesetround(previous); }
};
namespace {
float Add(float a, float b) {
    volatile float value = ps2_float_clamp(a) + ps2_float_clamp(b);
    return ps2_float_clamp(value);
}
float Sub(float a, float b) {
    volatile float value = ps2_float_clamp(a) - ps2_float_clamp(b);
    return ps2_float_clamp(value);
}
float Mul(float a, float b) {
    volatile float value = ps2_float_clamp(a) * ps2_float_clamp(b);
    return ps2_float_clamp(value);
}
size_t Index(const TextLayout::Font& font, uint8_t character) {
    const int64_t index = int64_t(character) - font.first_character;
    return index < 0 || uint64_t(index) >= font.glyphs.size() ? 0 : size_t(index) & 255;
}
}
uint8_t TextLayout::Page(const Glyph& glyph) {
    uint32_t bits;
    std::memcpy(&bits, &glyph.u, sizeof(bits));
    return uint8_t(bits & 3);
}
TextLayout::Result TextLayout::Build(const Font& font, int32_t frame_width, int32_t frame_height,
                                     const Item& item, Output& out) {
    if (font.glyphs.empty() || font.glyphs.size() > 256 || !std::isfinite(font.width) ||
        !std::isfinite(font.height) || frame_width <= 0 || frame_width > 4096 ||
        frame_height <= 0 || frame_height > 4096) return Result::InvalidFont;
    for (const auto& glyph : font.glyphs)
        if (!std::isfinite(glyph.u) || !std::isfinite(glyph.v) ||
            !std::isfinite(glyph.width) || !std::isfinite(glyph.height)) return Result::InvalidFont;
    if (!std::isfinite(item.x) || !std::isfinite(item.y) ||
        !std::isfinite(item.scale_x) || !std::isfinite(item.scale_y) ||
        item.text.find('\0') != std::string::npos) return Result::InvalidItem;
    if (item.text.size() > kMaxGlyphs + kMaxLines) return Result::LimitExceeded;
    Rounding rounding;
    const auto& reference = font.glyphs[Index(font, 'a')];
    const float unit_x = ps2_div_s(Mul(reference.width, 0.0625f), float(frame_width));
    const float unit_y = ps2_div_s(Mul(reference.height, 0.0625f), float(frame_height));
    if (!unit_x || !unit_y) return Result::InvalidFont;
    Output result;
    result.scale_x = Mul(item.scale_x, ps2_div_s(font.width, unit_x));
    result.scale_y = Mul(item.scale_y, ps2_div_s(font.height, unit_y));
    result.colour = item.colour;
    const float space_height = font.glyphs[Index(font, ' ')].height;
    size_t at = 0, glyph_count = 0;
    while (at < item.text.size()) {
        if (item.text[at] == '~') {
            ++at;
            const float extra = Mul(space_height, result.scale_y);
            result.height = Add(result.height, extra);
            // Before the first nonempty line the source updates an
            // uninitialised slot subsequently overwritten; only total height
            // is observable. No stack garbage is imported here.
            if (!result.lines.empty()) result.lines.back().height = Add(result.lines.back().height, extra);
            continue;
        }
        if (result.lines.size() == kMaxLines) return Result::LimitExceeded;
        Line line;
        float width = 0, height = space_height;
        while (at < item.text.size() && item.text[at] != '~') {
            if (++glyph_count > kMaxGlyphs) return Result::LimitExceeded;
            const size_t index = Index(font, uint8_t(item.text[at++]));
            const auto& glyph = font.glyphs[index];
            line.glyphs.push_back(uint8_t(index));
            width = Add(width, glyph.width);
            if (height < glyph.height) height = glyph.height;
            result.pages[Page(glyph)] = true;
        }
        if (at < item.text.size()) ++at;
        line.width = Mul(width, result.scale_x);
        line.height = Mul(height, result.scale_y);
        result.height = Add(result.height, line.height);
        while (line.glyphs.size() & 3) line.glyphs.push_back(0);
        result.lines.push_back(std::move(line));
    }
    float x = Mul(Mul(item.x, float(frame_width)), 16);
    float y = Mul(Mul(item.y, float(frame_height)), 16);
    if (!(item.alignment & 1)) y = Sub(y, item.alignment & 4 ? result.height : Mul(result.height, .5f));
    for (auto& line : result.lines) {
        line.x = x;
        if (!(item.alignment & 0x10)) line.x = Sub(x, item.alignment & 0x40 ? line.width : Mul(line.width, .5f));
        line.y = y;
        y = Add(y, line.height);
    }
    size_t first = 0;
    while (first < result.lines.size()) {
        size_t count = 0, bytes = 0;
        while (first + count < result.lines.size()) {
            const size_t next = bytes + result.lines[first + count].glyphs.size();
            if (next + count * 4 > 0x260) break;
            bytes = next;
            ++count;
        }
        if (!count) return Result::LimitExceeded;
        result.groups.push_back({first, count});
        first += count;
    }
    out = std::move(result);
    return Result::Ok;
}
}  // namespace title
