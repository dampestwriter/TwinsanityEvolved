#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/material_certificate.h"

namespace title {

// Private generated-data partitions; consumers use MaterialCertificate.
class MaterialCatalogue {
public:
    struct Accepted { const MaterialCertificate::Entry* data; size_t size; };
    struct Refusal { uint32_t material, reasons; };
    struct Refused { const Refusal* data; size_t size; };
    static MaterialCertificate::Archive Identity();
    static Accepted Accepted0();
    static Accepted Accepted1();
    static Accepted Accepted2();
    static Accepted Accepted3();
    static Refused Refused0();
    static Refused Refused1();
    static Refused Refused2();
    static Refused Refused3();
    static Refused Refused4();
    static Refused Refused5();
};

}  // namespace title
