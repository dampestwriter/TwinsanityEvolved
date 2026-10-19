#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_codec.h"

namespace title {

// Type-A sky assets are indexed images authored inside a 64-pixel-wide
// storage image. Convert that file layout at load time into owned RGBA8.
// No game-frame slot, VRAM mirror, GS state, or transfer executor is retained.
class SkyTexture {
public:
    struct Image {
        uint32_t width = 0, height = 0, buffer_width = 0;
        uint8_t levels = 0, function = 0;
        bool alpha = false;
        std::vector<uint8_t> rgba;
    };
    enum class Result { Ok, InvalidAsset, UnsupportedFormat };
    // header: TR renderer.h Texture's first 0x60 bytes, either the authored
    // header or the loaded object's header. Game-owned trailing pointers are
    // ignored. storage is TextureRead's serialized data after the 0x20 tool
    // leftovers. The exact fixed layout is checked before conversion.
    // Sky's type-A minifier uses level zero; inactive authored levels remain
    // metadata. Other families needing mip sampling require a separate reader.
    static Result Decode(const uint8_t* header, size_t header_size,
                         const uint8_t* storage, size_t storage_size, Image& out);
    // The same family check on the loaded object's header, with the pixels
    // taken from the record's CONVERTED image (TextureBindings::Read's
    // TextureCodec asset, textures at load) instead of the storage bytes:
    // level zero of an indexed asset whose size the header declares. The
    // codec's RGBA8 is the decoder's, so the result is
    // Decode's for the same record.
    static Result FromConverted(const uint8_t* header, size_t header_size,
                                const TextureCodec::Image& converted, Image& out);
};

}  // namespace title
