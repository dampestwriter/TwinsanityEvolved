#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/primitive_material.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_draw.h"

namespace title {

class TextIntent {
public:
    static constexpr uint32_t kDrawFontTexts = 0x19d138;
    static constexpr uint32_t kBegin = 0x1ac7a0, kDraw = 0x1ac7c0, kEnd = 0x1aa900;
    static constexpr uint32_t kRendererExtent = 0x30a788;
    static constexpr size_t kMaxItems = 256, kMaxBatchGlyphs = 65536;
    enum class Result {
        Ok, EmptyQueue, InvalidMemory, UnsupportedFont, InvalidItem,
        UnsupportedMaterial, LimitExceeded, InvalidGeometry
    };
    struct Page {
        uint32_t material = 0, shader = 0, bucket = 0;
        uint8_t page = 0;
        TextureBindings::Material sampling;
        std::vector<ps2r::gs::DrawIr> draws;
    };
    struct Output {
        uint32_t font = 0, source_items = 0;
        // First-touch order: page 0, then each used additional page. Root must
        // preserve source material-writer order across batches until scene end.
        // A page with no draw still has its source material lifecycle.
        std::vector<Page> pages;
    };
    static Result Capture(const PrimitiveMaterial::Read& read, const TextureBindings& textures,
                          uint32_t font_texts, const MaterialState::Environment& environment,
                          Output& out);

private:
    struct Font;
    static Result ReadFont(const PrimitiveMaterial::Read&, const TextureBindings&, uint32_t,
                           const MaterialState::Environment&, Font&);
    static Result ReadItem(const PrimitiveMaterial::Read&, uint32_t, TextLayout::Item&);
};

}  // namespace title
