#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/material_certificate.h"

#include <algorithm>
#include <array>
#include <iterator>

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/material_catalogue.h"

namespace title {
namespace {
using Catalogue = MaterialCatalogue;
const std::array<Catalogue::Accepted, 4>& Accepted() {
    static const std::array<Catalogue::Accepted, 4> chunks{
        Catalogue::Accepted0(), Catalogue::Accepted1(), Catalogue::Accepted2(), Catalogue::Accepted3()};
    return chunks;
}
const std::array<Catalogue::Refused, 6>& Refused() {
    static const std::array<Catalogue::Refused, 6> chunks{
        Catalogue::Refused0(), Catalogue::Refused1(), Catalogue::Refused2(),
        Catalogue::Refused3(), Catalogue::Refused4(), Catalogue::Refused5()};
    return chunks;
}
}  // namespace

MaterialCertificate::Archive MaterialCertificate::RetailArchive() { return Catalogue::Identity(); }
size_t MaterialCertificate::Size() {
    size_t size = 0;
    for (const auto& chunk : Accepted()) size += chunk.size;
    return size;
}
const MaterialCertificate::Entry* MaterialCertificate::At(size_t index) {
    for (const auto& chunk : Accepted()) {
        if (index < chunk.size) return chunk.data + index;
        index -= chunk.size;
    }
    return nullptr;
}
MaterialCertificate::Decision MaterialCertificate::Inspect(const Archive& observed,
    uint32_t material, std::string_view digest) {
    const auto archive = RetailArchive();
    if (observed.header_sha256 != archive.header_sha256 || observed.data_sha256 != archive.data_sha256)
        return {Result::UnverifiedArchive, nullptr, 0};
    for (const auto& chunk : Refused()) {
        const auto refused = std::lower_bound(chunk.data, chunk.data + chunk.size, material,
            [](const Catalogue::Refusal& value, uint32_t id) { return value.material < id; });
        if (refused != chunk.data + chunk.size && refused->material == material)
            return {Result::UnownedConsumer, nullptr, refused->reasons};
    }
    bool known = false;
    for (const auto& chunk : Accepted()) {
        auto accepted = std::lower_bound(chunk.data, chunk.data + chunk.size, material,
            [](const Entry& value, uint32_t id) { return value.material < id; });
        for (; accepted != chunk.data + chunk.size && accepted->material == material; ++accepted) {
            known = true;
            if (digest == accepted->record_sha256) return {Result::Certified, accepted, 0};
        }
    }
    return {known ? Result::UnknownRecord : Result::UnknownMaterial, nullptr, 0};
}
}  // namespace title
