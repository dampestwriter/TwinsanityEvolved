#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_service.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"

class TextFixture {
public:
    static constexpr uint32_t font = 0x140000, queue = 0x141000, pointers = 0x142000;
    static constexpr uint32_t glyphs = 0x144000, material = 0x150000, resources = 0x156000;
    static constexpr uint32_t item = 0x180000, strings = 0x190000;
    TextureBindingFixture textures;
    title::TextLayout::Font data;
    explicit TextFixture(uint32_t pages = 3);
    void SetFont(const title::TextLayout::Font&);
    void SetItem(uint32_t index, const title::TextLayout::Item&);
    void PutFloat(uint32_t, float);
    // RAM, and the textures fixture's scratchpad for the title's stack
    // (TR entry.cpp:83) unless `scratchpad` is false (a windowless reader).
    title::PrimitiveMaterial::Read Reader(bool scratchpad = true);
};
