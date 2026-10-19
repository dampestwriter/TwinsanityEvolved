#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_consumers.h"

namespace title {

// Private generated-data partitions; consumers use TextureConsumers.
class TextureConsumersCatalogue {
public:
    struct Rows { const TextureConsumers::Entry* data; size_t size; };
    static MaterialCertificate::Archive Identity();
    static Rows Rows0();
    static Rows Rows1();
    static Rows Rows2();
    static Rows Rows3();
    static Rows Rows4();
};

}  // namespace title
