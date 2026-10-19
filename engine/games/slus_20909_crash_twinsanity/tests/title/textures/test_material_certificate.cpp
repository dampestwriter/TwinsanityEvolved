#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/material_certificate.h"

namespace {
size_t checks = 0;
void Check(bool result) {
    ++checks;
    if (!result) {
        std::cerr << "material certificate check " << checks << " failed\n";
        std::exit(1);
    }
}
}  // namespace

int main() {
    using C = title::MaterialCertificate;
    using R = C::Result;
    const auto archive = C::RetailArchive();
    Check(C::Size() >= 3517);
    Check(C::At(C::Size()) == nullptr);
    uint32_t previous = 0;
    std::set<uint32_t> ids;
    size_t shared = 0, exclusive = 0;
    for (size_t i = 0; i != C::Size(); ++i) {
        const auto* entry = C::At(i);
        Check(entry && entry->material >= previous && entry->record_sha256.size() == 64);
        previous = entry->material;
        ids.insert(entry->material);
        const auto accepted = C::Inspect(archive, entry->material, entry->record_sha256);
        Check(accepted.result == R::Certified && accepted.entry == entry && !accepted.reasons);
        if (entry->texture_exclusive) ++exclusive; else ++shared;
        // BUG-010/082: an old pointer/ID or a new record with a reused ID does
        // not establish load identity. Generation receipts remain root-owned.
        std::string changed(entry->record_sha256);
        changed[0] = '!';
        Check(C::Inspect(archive, entry->material, changed).result == R::UnknownRecord);
        Check(C::Inspect(archive, entry->material, {}).result == R::UnknownRecord);
        // Complete archive bytes matter: its directory hash alone is not proof.
        Check(C::Inspect({archive.header_sha256, {}}, entry->material, entry->record_sha256).result ==
              R::UnverifiedArchive);
        Check(C::Inspect({{}, archive.data_sha256}, entry->material, entry->record_sha256).result ==
              R::UnverifiedArchive);
    }
    Check(ids.size() == 3517);
    Check(shared && exclusive);
    Check(C::Inspect(archive, 0xffffffff, "").result == R::UnknownMaterial);
    // Known UI material and actual shadow-default asset are never mesh-owned.
    const auto font = C::Inspect(archive, 0xb5cd358d, "");
    Check(font.result == R::UnownedConsumer && (font.reasons & C::Font));
    const auto shadow = C::Inspect(archive, 0x791fa4a8, "");
    Check(shadow.result == R::UnownedConsumer && (shadow.reasons & C::ShadowDefault));
    const auto skin = C::Inspect(archive, 0x000876ae, "");
    Check(skin.result == R::UnownedConsumer && (skin.reasons & C::Skin));
    const auto opaque = C::Inspect(archive, 0x0040598c, "");
    Check(opaque.result == R::UnownedConsumer && (opaque.reasons & C::OpaqueReference));
    Check(C::Inspect({}, 0xb5cd358d, "").result == R::UnverifiedArchive);
    std::cout << checks << " checks; " << ids.size() << " material IDs; "
              << shared << " shared / " << exclusive << " exclusive texture entries\n";
}
