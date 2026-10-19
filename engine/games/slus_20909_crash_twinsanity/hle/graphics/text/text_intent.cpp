#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_intent.h"

#include <array>
#include <cstring>
#include <utility>

namespace title {
struct TextIntent::Font {
    TextLayout::Font glyphs;
    int32_t width = 0, height = 0;
    std::vector<PrimitiveMaterial::Output> materials;
    std::vector<uint32_t> addresses;
};
namespace {
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
float Float(const uint8_t* p) {
    const uint32_t bits = Word(p);
    float result; std::memcpy(&result, &bits, sizeof(result)); return result;
}
// Main RAM in any KUSEG/KSEG0/KSEG1 alias, or the 16 KB scratchpad exactly
// (0x70000000..0x70003fff): the title's main thread runs on the scratchpad
// (TR entry.cpp:83; stack.h:5-8), so a queued item or its string may be a
// stack object there. The reader decides whether that window exists; the
// same rule as SpriteIntent/TextureBindings.
bool Read(const PrimitiveMaterial::Read& read, uint32_t address, void* out, size_t size) {
    if (!read) return false;
    if ((address & 0xffffc000) == 0x70000000)
        return size <= 0x4000 - (address & 0x3fff) && read(address, out, size);
    const uint32_t segment = address & 0xe0000000, at = address & 0x1fffffff;
    return at && (segment == 0 || segment == 0x80000000 || segment == 0xa0000000) &&
        at < 0x2000000 && size <= 0x2000000 - at && read(address, out, size);
}
}
TextIntent::Result TextIntent::ReadFont(
    const PrimitiveMaterial::Read& read, const TextureBindings& textures, uint32_t address,
    const MaterialState::Environment& environment, Font& out) {
    std::array<uint8_t, 0x40> bytes{};
    std::array<uint8_t, 0x28> table{};
    if ((address & 3) || !Read(read, address, bytes.data(), bytes.size()) ||
        !Read(read, Word(bytes.data() + 0x14), table.data(), table.size())) return Result::InvalidMemory;
    constexpr uint32_t methods[]{kBegin, kDraw, kEnd};
    for (size_t n = 0; n < 3; ++n)
        if ((Word(table.data() + 0x10 + n * 8) & 0xffff) ||
            Word(table.data() + 0x14 + n * 8) != methods[n]) return Result::UnsupportedFont;
    const uint32_t count = Word(bytes.data()), pages = Word(bytes.data() + 0x18);
    if (!count || count > 256 || !pages || pages > 3) return Result::UnsupportedFont;
    std::array<uint8_t, 4> extent{};
    if (!Read(read, kRendererExtent, extent.data(), extent.size())) return Result::InvalidMemory;
    out.width = int16_t(uint16_t(extent[0]) | uint16_t(extent[1]) << 8);
    out.height = int16_t(uint16_t(extent[2]) | uint16_t(extent[3]) << 8);
    if (out.width <= 0 || out.width > 4096 || out.height <= 0 || out.height > 4096)
        return Result::UnsupportedFont;
    const uint32_t first = Word(bytes.data() + 4), glyphs = Word(bytes.data() + 8);
    std::memcpy(&out.glyphs.first_character, &first, sizeof(first));
    out.glyphs.width = Float(bytes.data() + 0xc); out.glyphs.height = Float(bytes.data() + 0x10);
    out.glyphs.glyphs.resize(count);
    if ((glyphs & 15) || !Read(read, glyphs, out.glyphs.glyphs.data(), count * 16))
        return Result::InvalidMemory;
    for (uint32_t page = 0; page < pages; ++page) {
        const uint32_t resource = Word(bytes.data() + 0x1c + page * 4);
        const uint32_t texture = Word(bytes.data() + 0x28 + page * 4);
        std::array<uint8_t, 12> material_resource{};
        if ((resource & 3) || !Read(read, resource, material_resource.data(), material_resource.size()))
            return Result::InvalidMemory;
        const uint32_t material = Word(material_resource.data() + 8);
        PrimitiveMaterial::Output captured;
        if (PrimitiveMaterial::Capture(read, textures, material, environment, captured,
                                       PrimitiveMaterial::Kind::Text) != PrimitiveMaterial::Result::Ok)
            return Result::UnsupportedMaterial;
        std::array<uint8_t, 1> source_page{};
        if (!Read(read, captured.shader + 0xc, source_page.data(), source_page.size()))
            return Result::InvalidMemory;
        const uint32_t texture_segment = texture & 0xe0000000;
        if (texture_segment != 0 && texture_segment != 0x80000000 && texture_segment != 0xa0000000)
            return Result::InvalidMemory;
        if (source_page[0] != page || !texture || (texture & 3) ||
            captured.sampling.asset.token.object != (texture & 0x1fffffff)) return Result::UnsupportedFont;
        out.materials.push_back(std::move(captured));
        out.addresses.push_back(material);
    }
    return Result::Ok;
}
TextIntent::Result TextIntent::ReadItem(const PrimitiveMaterial::Read& read, uint32_t address,
                                       TextLayout::Item& out) {
    std::array<uint8_t, 0x1c> bytes{};
    if ((address & 3) || !Read(read, address, bytes.data(), bytes.size())) return Result::InvalidMemory;
    out.x = Float(bytes.data()); out.y = Float(bytes.data() + 4);
    out.scale_x = Float(bytes.data() + 8); out.scale_y = Float(bytes.data() + 12);
    out.colour = Word(bytes.data() + 0x10); out.alignment = Word(bytes.data() + 0x14);
    const uint32_t text = Word(bytes.data() + 0x18);
    for (size_t n = 0; n <= TextLayout::kMaxGlyphs + TextLayout::kMaxLines; ++n) {
        uint8_t c = 0;
        if (uint64_t(text) + n > 0xffffffff || !Read(read, text + uint32_t(n), &c, 1))
            return Result::InvalidMemory;
        if (!c) return Result::Ok;
        out.text.push_back(char(c));
    }
    return Result::LimitExceeded;
}
TextIntent::Result TextIntent::Capture(
    const PrimitiveMaterial::Read& read, const TextureBindings& textures, uint32_t font_texts,
    const MaterialState::Environment& environment, Output& out) {
    std::array<uint8_t, 0x14> queue{};
    if ((font_texts & 3) || !Read(read, font_texts, queue.data(), queue.size())) return Result::InvalidMemory;
    const uint32_t count = Word(queue.data() + 8);
    if (!count) return Result::EmptyQueue; // source returns before accessing the font
    if (count > kMaxItems || count > Word(queue.data() + 0xc)) return Result::LimitExceeded;
    Font font;
    const auto captured = ReadFont(read, textures, Word(queue.data()), environment, font);
    if (captured != Result::Ok) return captured;
    const uint32_t pointers = Word(queue.data() + 4);
    std::vector<uint8_t> items(count * 4);
    if ((pointers & 3) || !Read(read, pointers, items.data(), items.size())) return Result::InvalidMemory;
    std::vector<TextLayout::Output> layouts;
    std::array<bool, 4> pages{};
    size_t glyphs = 0, packets = 0;
    for (uint32_t item = 0; item < count; ++item) {
        TextLayout::Item text;
        const auto status = ReadItem(read, Word(items.data() + item * 4), text);
        if (status != Result::Ok) return status;
        glyphs += text.text.size();
        if (glyphs > kMaxBatchGlyphs) return Result::LimitExceeded;
        TextLayout::Output layout;
        const auto result = TextLayout::Build(font.glyphs, font.width, font.height, text, layout);
        if (result == TextLayout::Result::LimitExceeded) return Result::LimitExceeded;
        if (result != TextLayout::Result::Ok) return Result::InvalidItem;
        packets += layout.groups.size();
        if (packets > 128) return Result::LimitExceeded;
        for (size_t page = 0; page < 4; ++page) pages[page] = pages[page] || layout.pages[page];
        layouts.push_back(std::move(layout));
    }
    Output result;
    result.font = Word(queue.data()) & 0x1fffffff; result.source_items = count;
    for (size_t page = 0; page < font.materials.size(); ++page) {
        if (page && !pages[page + 1]) continue;
        const auto& material = font.materials[page];
        Page output;
        output.material = font.addresses[page]; output.shader = material.shader;
        output.bucket = material.bucket; output.page = uint8_t(page);
        output.sampling = material.sampling;
        for (const auto& layout : layouts) {
            std::vector<ps2r::gs::DrawIr> draws;
            if (TextDraw::Build(material.draw, font.glyphs, layout, uint8_t(page + 1),
                                font.width, font.height, draws) != TextDraw::Result::Ok)
                return Result::InvalidGeometry;
            for (auto& draw : draws) output.draws.push_back(std::move(draw));
        }
        result.pages.push_back(std::move(output));
    }
    out = std::move(result);
    return Result::Ok;
}
}  // namespace title
