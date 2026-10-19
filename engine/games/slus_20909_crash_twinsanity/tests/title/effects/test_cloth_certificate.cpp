#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_certificate.h"

#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("cloth certificate line ") + std::to_string(__LINE__)); } while (0)
}
int main() {
    try {
        using C = title::ClothCertificate;
        const auto archive = title::MaterialCertificate::RetailArchive();
        std::set<uint32_t> ids;
        unsigned ordinary = 0, single = 0, three = 0;
        uint32_t previous = 0;
        CHECK(C::Size() == 53 && !C::At(53));
        for (size_t i = 0; i < C::Size(); ++i) {
            const auto* entry = C::At(i);
            CHECK(entry && entry->material >= previous && entry->texture && entry->record_sha256.size() == 64);
            previous = entry->material;
            ids.insert(entry->material);
            const auto accepted = C::Inspect(archive, entry->material, entry->record_sha256);
            CHECK(accepted.result == C::Result::Certified && accepted.entry == entry);
            CHECK(C::Inspect(archive, entry->material, {}).result == C::Result::UnknownRecord);
            auto changed = std::string(entry->record_sha256);
            changed[0] = '!';
            CHECK(C::Inspect(archive, entry->material, changed).result == C::Result::UnknownRecord);
            CHECK(C::Inspect({archive.header_sha256, {}}, entry->material, entry->record_sha256).result ==
                  C::Result::UnverifiedArchive);
            CHECK(C::Inspect({{}, archive.data_sha256}, entry->material, entry->record_sha256).result ==
                  C::Result::UnverifiedArchive);
            if (entry->kind == C::Kind::Ordinary) ++ordinary;
            else if (entry->kind == C::Kind::Single) ++single;
            else if (entry->kind == C::Kind::Three) ++three;
            else CHECK(false);
        }
        CHECK(ids.size() == 50 && ordinary == 3 && single == 46 && three == 4);
        CHECK(C::Inspect(archive, 0xffffffff, {}).result == C::Result::UnknownMaterial);
        CHECK(C::Inspect(archive, 0x791fa4a8, {}).result == C::Result::UnknownMaterial); // shadow
        CHECK(C::Inspect(archive, 0xb5cd358d, {}).result == C::Result::UnknownMaterial); // font
        std::printf("ClothCertificate: %u checks, 50 IDs/53 exact versions; absent receipts never select\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
