#include "games/slus_20909_crash_twinsanity/tests/title/text/text_fixture.h"

#include <cstring>
#include <stdexcept>

TextFixture::TextFixture(uint32_t pages) {
    auto& f = textures;
    for (uint32_t page = 0; page < pages; ++page) {
        const uint32_t object = f.object + page * 0x100;
        f.Put(object + 4, 19 + page); f.Put(object + 0x60, 0xffffffff);
        f.Put(f.stream + 12, f.data);
        title::TextureAssets::View view;
        if (f.bindings.Read(object, f.stream, view) != title::TextureBindings::Result::Ok)
            throw std::runtime_error("font fixture texture load");
        f.Shader(13, -69, 0, true, false);
        const uint32_t shader = f.shader + page * 0x100;
        if (page) std::memcpy(f.ram.data() + shader, f.ram.data() + f.shader, 0x70);
        f.Put(shader + 0xc, page, 1); f.Put(shader + 0x14, object); f.Put(shader + 0x60, 19 + page);
        // Mix, texturing, depth ALWAYS, no writes. Font's shared call preserves alpha.
        f.Put(shader, uint64_t{1} | uint64_t{1} << 21 | uint64_t{1} << 27 |
              uint64_t{1} << 55 | uint64_t{1} << 59, 8);
        f.Put(material + page * 0x100, shader);
        f.Put(material + page * 0x100 + 0x10, 1);
        f.Put(material + page * 0x100 + 0x18, 0x10000000);
        f.Put(material + page * 0x100 + 0x20, 26);
        f.Put(material + page * 0x100 + 0x60, 2);
        f.Put(resources + page * 0x10 + 8, material + page * 0x100);
        f.Put(font + 0x1c + page * 4, resources + page * 0x10);
        f.Put(font + 0x28 + page * 4, object);
    }
    // Shader() uses the base address as a scratch template; restore page 0.
    f.Put(f.shader + 0xc, 0, 1); f.Put(f.shader + 0x14, f.object); f.Put(f.shader + 0x60, 19);
    f.Put(f.shader, uint64_t{1} | uint64_t{1} << 21 | uint64_t{1} << 27 |
          uint64_t{1} << 55 | uint64_t{1} << 59, 8);
    f.Put(font + 0x14, 0x2f67e0); f.Put(font + 0x18, pages);
    f.Put(queue, font); f.Put(queue + 4, pointers); f.Put(queue + 0xc, 256);
    f.Put(title::MaterialState::kDisplayWidth, 512); f.Put(title::MaterialState::kDisplayHeight, 448);
    f.Put(title::MaterialState::kDisplayPixelBytes, 2); f.Put(title::MaterialState::kDepthBufferPage, 168);
    f.Put(title::TextIntent::kRendererExtent, 512, 2); f.Put(title::TextIntent::kRendererExtent + 2, 448, 2);
    data.first_character = 32; data.width = .02f; data.height = .03f;
    data.glyphs.resize(160);
    for (size_t n = 0; n < data.glyphs.size(); ++n) {
        auto& glyph = data.glyphs[n];
        glyph = {float((n % 16) * 64), float(2048 - (n / 16) * 128), float(48 + n % 7), float(96 + n % 13)};
        uint32_t bits; std::memcpy(&bits, &glyph.u, 4);
        bits = (bits & ~3u) | (n == 0 ? 0 : n % pages + 1);
        std::memcpy(&glyph.u, &bits, 4);
    }
    SetFont(data);
}
void TextFixture::PutFloat(uint32_t at, float value) {
    uint32_t bits; std::memcpy(&bits, &value, 4); textures.Put(at, bits);
}
void TextFixture::SetFont(const title::TextLayout::Font& value) {
    data = value;
    textures.Put(font, data.glyphs.size()); textures.Put(font + 4, uint32_t(data.first_character));
    textures.Put(font + 8, glyphs); PutFloat(font + 0xc, data.width); PutFloat(font + 0x10, data.height);
    std::memcpy(textures.ram.data() + glyphs, data.glyphs.data(), data.glyphs.size() * 16);
}
void TextFixture::SetItem(uint32_t index, const title::TextLayout::Item& value) {
    if (index >= 64 || value.text.size() >= 0x900) throw std::runtime_error("fixture text bound");
    const uint32_t at = item + index * 0x40, text = strings + index * 0x900;
    textures.Put(pointers + index * 4, at);
    if (textures.Word(queue + 8) <= index) textures.Put(queue + 8, index + 1);
    PutFloat(at, value.x); PutFloat(at + 4, value.y);
    PutFloat(at + 8, value.scale_x); PutFloat(at + 12, value.scale_y);
    textures.Put(at + 0x10, value.colour); textures.Put(at + 0x14, value.alignment);
    textures.Put(at + 0x18, text);
    std::memcpy(textures.ram.data() + text, value.text.c_str(), value.text.size() + 1);
}
title::PrimitiveMaterial::Read TextFixture::Reader(bool scratchpad) {
    return [this, scratchpad](uint32_t address, void* out, size_t size) {
        if ((address & 0xffffc000) == 0x70000000) {
            const uint32_t offset = address & 0x3fff;
            if (!scratchpad || size > textures.scratchpad.size() - offset) return false;
            std::memcpy(out, textures.scratchpad.data() + offset, size); return true;
        }
        const uint32_t at = address & 0x1fffffff;
        if (at > textures.ram.size() || size > textures.ram.size() - at) return false;
        std::memcpy(out, textures.ram.data() + at, size); return true;
    };
}
