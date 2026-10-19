#pragma once

#include <cstddef>
#include <cstdint>

namespace title {

// The complete retail UI pair catalogue: every PSM picture/icon and PSF font
// page declares one texture ID with the one material that draws it (a
// one-shader type-0D material in the UI (24) or font (26) bucket, program key
// 0x10000000, the pair's own texture -- TR draw2d.cpp Begin2D draws
// shaders[0]). 2,578 pictures + 4 font pages; no texture or material appears
// in two pairs. Static archive data generated and checked by
// tests/title/textures/audit_texture_pairs.py; it selects nothing by itself:
// TextureSelection decides ownership per ID with TextureConsumers (the
// families and shader types that still read the texture).
class TexturePairs {
public:
    enum class Kind : uint8_t { Picture, Font };
    struct Entry { uint32_t texture, material; Kind kind; };
    static const char* ArchiveHeaderSha256();
    static const char* ArchiveDataSha256();
    static size_t Size();
    static const Entry* At(size_t index);
    static const Entry* Find(uint32_t texture);
    static const Entry* FindMaterial(uint32_t material);
};

}  // namespace title
