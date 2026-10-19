#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/material_certificate.h"

namespace title {

// Complete placed-mesh consumer proof for the cloth material IDs, including
// their three ordinary-material versions. It grants no texture/target ownership.
class ClothCertificate {
public:
    using Archive = MaterialCertificate::Archive;
    enum class Kind { Ordinary = 1, Single = 0x17, Three = 0x1a };
    struct Entry {
        uint32_t material, texture;
        Kind kind;
        std::string_view record_sha256;
    };
    enum class Result { Certified, UnverifiedArchive, UnknownMaterial, UnknownRecord };
    struct Decision { Result result; const Entry* entry = nullptr; };
    static size_t Size();
    static const Entry* At(size_t index);
    // Observed archive hashes and the complete original material record digest
    // must come from the file owner, bound to the current object generation.
    // ID, shader type, object address or a runtime seen-set are insufficient.
    // Root must also prove every part of the submitted model has an owner,
    // and that this material/texture/frame generation remains the captured one.
    static Decision Inspect(const Archive& observed, uint32_t material,
                            std::string_view original_record_sha256);
};

}  // namespace title
