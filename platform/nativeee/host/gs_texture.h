// gs_texture.h -- a game texture resource (its PS2 Texture header + the GS upload packet the file carries) decoded to
// RGBA8 through the engine's GS model: the packet's transfers are executed into a private VramImage by the engine's
// TransferEngine (the real swizzle: the tools upload the texels in one format and the GS reads them in another), then
// TextureDecoder reads level 0 through the TEX0 view the PS2 renderer would have written (WriteTexturePairs), and each
// further level through the same view at that level's address and width (the MIPTBP1/2 the renderer wrote).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "te_gfx_layout.h"

namespace te::gfx {

struct DecodedTexture {
    int width = 0, height = 0;
    std::vector<uint32_t> rgba;   // GPU alpha (0xFF opaque), row 0 = the texture's top
    std::vector<std::vector<uint32_t>> mips;   // the levels below the first (level i: (width >> i) x (height >> i))
    uint32_t transfers = 0;       // host->local transfers the packet started
    uint32_t image_bytes = 0;     // IMAGE bytes delivered
};

// `packet` is the texture's transfer chain as the file stores it (DMA tags with VIF codes, GIF A+D and IMAGE data),
// `size` its byte length. False with *error on a chain the walker cannot follow.
bool DecodeTextureResource(const te_gfx_texture_header& header, const uint8_t* packet, size_t size,
                           DecodedTexture& out, std::string* error);

}  // namespace te::gfx
