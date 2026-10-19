#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pairs.h"

namespace title {

// Private generated-data partitions; consumers use TexturePairs.
class TexturePairsCatalogue {
public:
    struct Rows { const TexturePairs::Entry* data; const uint16_t* by_material; size_t size; };
    static constexpr size_t kChunks = 4;
    static const char* HeaderSha256();
    static const char* DataSha256();
    static Rows Chunk(size_t index);
    static Rows Rows0();
    static Rows Rows1();
    static Rows Rows2();
    static Rows Rows3();
};

}  // namespace title
