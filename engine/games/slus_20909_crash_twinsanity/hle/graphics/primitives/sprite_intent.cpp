#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_intent.h"

#include <array>
#include <cstring>
#include <utility>

namespace title {
namespace {
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
// Main RAM in any KUSEG/KSEG0/KSEG1 alias, or the 16 KB scratchpad exactly
// (0x70000000..0x70003fff): the title's main thread runs on the scratchpad
// (TR entry.cpp:83), so a placed draw's matrix is a stack object there
// (particles2d.cpp:479/483). The reader decides whether that window exists.
bool ReadBytes(const PrimitiveMaterial::Read& read, uint32_t address, void* out, size_t size) {
    if (!read) return false;
    if ((address & 0xffffc000) == 0x70000000)
        return size <= 0x4000 - (address & 0x3fff) && read(address, out, size);
    const uint32_t physical = address & 0x1fffffff, segment = address & 0xe0000000;
    return physical && (segment == 0 || segment == 0x80000000 || segment == 0xa0000000) &&
        physical < 0x2000000 && size <= 0x2000000 - physical && read(address, out, size);
}
}
SpriteIntent::Result SpriteIntent::Capture(
    const PrimitiveMaterial::Read& read, const TextureBindings& textures, uint32_t entry,
    uint32_t a0, uint32_t a1, uint32_t a2, const MaterialState::Environment& environment, Output& out) {
    unsigned slot = 0;
    switch (entry) {
        case kPlain: slot = 5; break;
        case kColoured: slot = 6; break;
        case kPlaced: slot = 7; break;
        case kPlacedColoured: slot = 8; break;
        default: return Result::UnsupportedEntry;
    }
    std::array<uint8_t, 0x20> object{};
    std::array<uint8_t, 0x48> table{};
    if ((a0 & 3) || !ReadBytes(read, a0, object.data(), object.size()) ||
        !ReadBytes(read, Word(object.data() + 0xc), table.data(), table.size())) return Result::InvalidMemory;
    if (Word(table.data() + slot * 8) & 0xffff || Word(table.data() + slot * 8 + 4) != entry ||
        Word(table.data() + 0x40) & 0xffff || Word(table.data() + 0x44) != kPlacedColoured)
        return Result::UnsupportedOwner;
    SpriteDraw::Input input;
    std::memcpy(&input.texture, object.data() + 0x10, 16);
    input.turned = object[8] != 0;
    SpriteDraw::Matrix matrix{};
    if (entry == kPlaced || entry == kPlacedColoured) {
        if ((a1 & 15) || !ReadBytes(read, a1, &matrix, sizeof(matrix))) return Result::InvalidMemory;
        input.matrix = &matrix;
    }
    if (entry == kColoured) input.colour = a1;
    else if (entry == kPlacedColoured) input.colour = a2;
    else {
        // GetColor(ColourWhite): NEW generated 0x11bf10 uses 0x2e6fa8 + 15*4.
        std::array<uint8_t, 4> colour{};
        if (!ReadBytes(read, 0x2e6fe4, colour.data(), colour.size())) return Result::InvalidMemory;
        input.colour = Word(colour.data());
    }
    PrimitiveMaterial::Output material;
    if (PrimitiveMaterial::Capture(read, textures, Word(object.data()), environment, material) !=
        PrimitiveMaterial::Result::Ok) return Result::InvalidMaterial;
    Output result;
    if (SpriteDraw::Build(material.draw, material.target, input, result.draw) != SpriteDraw::Result::Ok)
        return Result::InvalidGeometry;
    // This still certifies the complete source material and converted texture
    // generation. The service can finish an empty producer without recording
    // GPU work; following selected draws supply their whole state and upload.
    const bool empty = result.draw.verts.empty();
    result.bucket = material.bucket;
    result.sampling = std::move(material.sampling);
    out = std::move(result);
    return empty ? Result::Empty : Result::Ok;
}
}  // namespace title
