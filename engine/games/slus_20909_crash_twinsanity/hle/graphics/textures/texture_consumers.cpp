#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_consumers.h"

#include <algorithm>
#include <array>

#include "engine/core/config/config.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_consumers_catalogue.h"

namespace title {
namespace {
using Catalogue = TextureConsumersCatalogue;
const std::array<Catalogue::Rows, 5>& Rows() {
    static const std::array<Catalogue::Rows, 5> chunks{
        Catalogue::Rows0(), Catalogue::Rows1(), Catalogue::Rows2(), Catalogue::Rows3(), Catalogue::Rows4()};
    return chunks;
}
}  // namespace

MaterialCertificate::Archive TextureConsumers::RetailArchive() { return Catalogue::Identity(); }
size_t TextureConsumers::Size() {
    size_t size = 0;
    for (const auto& chunk : Rows()) size += chunk.size;
    return size;
}
const TextureConsumers::Entry* TextureConsumers::At(size_t index) {
    for (const auto& chunk : Rows()) {
        if (index < chunk.size) return chunk.data + index;
        index -= chunk.size;
    }
    return nullptr;
}
const TextureConsumers::Entry* TextureConsumers::Find(uint32_t texture) {
    for (const auto& chunk : Rows()) {
        const auto found = std::lower_bound(chunk.data, chunk.data + chunk.size, texture,
            [](const Entry& value, uint32_t id) { return value.texture < id; });
        if (found != chunk.data + chunk.size && found->texture == texture) return found;
    }
    return nullptr;
}
TextureConsumers::Readiness TextureConsumers::Inspect(const MaterialCertificate::Archive& observed,
    uint32_t texture, uint32_t selected_families, uint64_t supported_shaders) {
    const auto archive = RetailArchive();
    if (observed.header_sha256 != archive.header_sha256 || observed.data_sha256 != archive.data_sha256)
        return {Result::UnverifiedArchive, nullptr, 0, 0};
    const auto* entry = Find(texture);
    if (!entry) return {Result::UnknownTexture, nullptr, 0, 0};
    Readiness readiness{Result::Retained, entry, 0, 0};
    readiness.retained_families = entry->families & ~(selected_families & ~uint32_t(Unownable));
    readiness.unsupported_shaders = entry->shaders & ~supported_shaders;
    if (!readiness.retained_families && !readiness.unsupported_shaders) readiness.result = Result::Owned;
    return readiness;
}
TextureConsumers::Selection TextureConsumers::Selected(const ps2r::HleConfig& hle) {
    constexpr auto Shader = [](unsigned type) { return uint64_t(1) << type; };
    Selection selection;
    // Fonts: TextService (PSF glyph pages, shader 0x0D). Pictures/icons:
    // SpriteService (PSM pairs, shader 0x0D; the retail flat/particle 0x0E
    // variant draws no serialized texture). Sky: SkyPass, type-A shader 0x0A
    // (TR shadersettings.cpp:156). Shadows: the default shapes, shader 0x0B,
    // reference no texture in retail (ShadowDefault count 0) but the family
    // bit is the owner's, so it is selected with the composed application.
    if (hle.graphics_text) { selection.families |= Font; selection.shaders |= Shader(0x0d); }
    if (hle.graphics_sprites) { selection.families |= Sprite; selection.shaders |= Shader(0x0d) | Shader(0x0e); }
    if (hle.graphics_sky) { selection.families |= Sky; selection.shaders |= Shader(0x0a); }
    if (hle.graphics_shadows) { selection.families |= ShadowDefault; selection.shaders |= Shader(0x0b); }
    return selection;
}
}  // namespace title
