#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace title {

// Immutable complete-retail-asset consumer proof for MeshPass. This is not a
// runtime seen-set, material-pointer cache, or permission to skip live guards.
// Source: renderer/resources.cpp, models.cpp; game/{shapes,oleg,shadows}.cpp.
class MaterialCertificate {
public:
    struct Archive {
        std::string_view header_sha256;
        std::string_view data_sha256;
    };
    struct Entry {
        uint32_t material;
        uint32_t texture;
        std::string_view record_sha256;
        // False => another retained family uses this texture. Even true does
        // not replace TextureSelection or authorize its load/lifetime cutover.
        bool texture_exclusive;
    };
    enum class Result {
        Certified, UnverifiedArchive, UnknownMaterial, UnownedConsumer, UnknownRecord
    };
    enum Reason : uint32_t {
        MaterialEnvelope = 1 << 0, TextureIdentityVaries = 1 << 1,
        Particle = 1 << 2, Sprite = 1 << 3, Font = 1 << 4,
        BorrowedHud = 1 << 5, ShadowDefault = 1 << 6, Skin = 1 << 7,
        BlendSkin = 1 << 8, Sky = 1 << 9, OpaqueReference = 1 << 10,
        MissingTexture = 1 << 11, MissingMaterial = 1 << 12,
        ModelNotQualified = 1 << 13, NoMeshConsumer = 1 << 14,
        SharedUnownedModel = 1 << 15
    };
    struct Decision {
        Result result = Result::UnknownMaterial;
        const Entry* entry = nullptr;
        uint32_t reasons = 0;
    };

    // Expected identities, for verification by the file owner. Never use
    // these constants as a substitute for hashing the actual mounted files.
    static Archive RetailArchive();
    static size_t Size();
    static const Entry* At(size_t index);

    // Caller supplies observed archive fingerprints and the complete ORIGINAL
    // MaterialResourceRead record digest, tied to the current resource's load
    // generation. Unknown/replaced files, ID-only lookups and missing receipts
    // cannot produce Certified. No argument or service state is mutated.
    //
    // A live MeshPass callback must ALSO prove current MaterialResource->Material
    // identity, no foreign resource injection, readiness of every related model
    // and texture generation, and the source bucket's environment. Certificates
    // cover rigid/placed ONLY. Screen/default/skin/sky/UI stay separate owners.
    static Decision Inspect(const Archive& observed, uint32_t material,
                            std::string_view original_record_sha256);
};

}  // namespace title
