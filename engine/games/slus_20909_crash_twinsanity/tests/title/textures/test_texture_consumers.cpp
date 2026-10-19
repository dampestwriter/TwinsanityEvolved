#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_consumers.h"

#include <cstdio>
#include <stdexcept>
#include <string>

#include "engine/core/config/config.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/material_certificate.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_owners.h"

namespace {
using TC = title::TextureConsumers;
using MC = title::MaterialCertificate;
using TO = title::TextureOwners;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("texture consumers line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
constexpr uint64_t Shader(unsigned type) { return uint64_t(1) << type; }

void Catalogue() {
    const auto size = TC::Size();
    CHECK(size == 4048);
    uint32_t previous = 0;
    size_t certified = 0, exclusive = 0, unownable = 0;
    for (size_t i = 0; i < size; ++i) {
        const auto* entry = TC::At(i);
        CHECK(entry && (i == 0 || entry->texture > previous));
        CHECK(entry->families != 0 && entry->shaders != 0 && entry->materials > 0);
        CHECK((entry->families & TC::Unreferenced) == 0 && (entry->families & TC::MissingRecord) == 0);
        CHECK(entry->appearances > 0);
        CHECK(TC::Find(entry->texture) == entry);
        if (entry->families & TC::CertifiedPlaced) ++certified;
        if (entry->families == TC::CertifiedPlaced) ++exclusive;
        if (entry->families & TC::Unownable) ++unownable;
        // Cloth is both a consumer family and a shader identity; they agree.
        CHECK(bool(entry->families & TC::Cloth) == bool(entry->shaders & (Shader(0x17) | Shader(0x1a))));
        previous = entry->texture;
    }
    CHECK(TC::At(size) == nullptr && TC::Find(0) == nullptr && TC::Find(0xffffffffu) == nullptr);
    CHECK(certified == 835 && exclusive == 315 && unownable == 33);
}

void AgreesWithCertificate() {
    // Every certified material's texture is a certified-placed consumer, and
    // the certificate's one-bit exclusivity equals "no other family at all".
    const auto archive = MC::RetailArchive();
    CHECK(archive.header_sha256 == TC::RetailArchive().header_sha256);
    CHECK(archive.data_sha256 == TC::RetailArchive().data_sha256);
    for (size_t i = 0; i < MC::Size(); ++i) {
        const auto* material = MC::At(i);
        const auto* texture = TC::Find(material->texture);
        CHECK(texture && (texture->families & TC::CertifiedPlaced));
        CHECK(material->texture_exclusive == (texture->families == TC::CertifiedPlaced));
        CHECK(texture->shaders & Shader(0x01));
        const auto ready = TC::Inspect(archive, material->texture, TC::CertifiedPlaced, Shader(0x01));
        CHECK(ready.entry == texture);
        CHECK((ready.result == TC::Result::Owned) == material->texture_exclusive);
        if (!material->texture_exclusive) {
            CHECK(ready.result == TC::Result::Retained);
            CHECK(ready.retained_families != 0 || ready.unsupported_shaders != 0);
            CHECK((ready.retained_families & TC::CertifiedPlaced) == 0);
        }
    }
}

void AgreesWithStartupOwners() {
    for (const auto& owner : TO::kStartup) {
        const auto* entry = TC::Find(owner.texture);
        CHECK(entry != nullptr);
        const bool font = owner.family == TO::Family::CrashFont || owner.family == TO::Family::ArialFont;
        CHECK(bool(entry->families & TC::Font) == font);
        CHECK(bool(entry->families & TC::Sprite) == !font);
        CHECK(entry->shaders & Shader(0x0d));
        // The startup UI pages share no world mesh consumer.
        CHECK((entry->families & (TC::CertifiedPlaced | TC::Placed | TC::Rigid | TC::Skin)) == 0);
    }
}

void InspectSemantics() {
    const auto archive = TC::RetailArchive();
    MC::Archive foreign{archive.header_sha256, "0"};
    CHECK(TC::Inspect(foreign, TC::At(0)->texture, ~0u, ~uint64_t(0)).result == TC::Result::UnverifiedArchive);
    CHECK(TC::Inspect(archive, 0, ~0u, ~uint64_t(0)).result == TC::Result::UnknownTexture);
    size_t owned_all = 0, retained_by_shader = 0;
    for (size_t i = 0; i < TC::Size(); ++i) {
        const auto* entry = TC::At(i);
        // Selecting every ownable family and every shader owns exactly the
        // textures without an unownable family; nothing else is ever Owned.
        const auto all = TC::Inspect(archive, entry->texture, ~0u, ~uint64_t(0));
        CHECK(all.entry == entry);
        CHECK((all.result == TC::Result::Owned) == !(entry->families & TC::Unownable));
        CHECK(all.retained_families == (entry->families & TC::Unownable));
        if (all.result == TC::Result::Owned) ++owned_all;
        // Nothing is owned when nothing is selected.
        const auto none = TC::Inspect(archive, entry->texture, 0, 0);
        CHECK(none.result == TC::Result::Retained && none.retained_families == entry->families);
        CHECK(none.unsupported_shaders == entry->shaders);
        // Family selection alone never grants ownership of an unsupported shader.
        const auto families_only = TC::Inspect(archive, entry->texture, ~0u, 0);
        CHECK(families_only.result == TC::Result::Retained && families_only.unsupported_shaders == entry->shaders);
        if (!(entry->families & TC::Unownable)) ++retained_by_shader;
    }
    CHECK(owned_all == TC::Size() - 33 && retained_by_shader == owned_all);
    // Sprite/font selection with type 0D supports the startup pages only.
    const auto crash = TC::Inspect(archive, 0x922ed81e, TC::Font, Shader(0x0d));
    CHECK(crash.result == TC::Result::Owned);
    const auto icon = TC::Inspect(archive, 0x046a8347, TC::Sprite, Shader(0x0d));
    CHECK(icon.result == TC::Result::Owned);
    CHECK(TC::Inspect(archive, 0x046a8347, TC::Font, Shader(0x0d)).result == TC::Result::Retained);
    CHECK(TC::Inspect(archive, 0x046a8347, TC::Sprite, Shader(0x01)).result == TC::Result::Retained);
}

void ClothAndSharedWorld() {
    const auto archive = TC::RetailArchive();
    size_t cloth = 0, cloth_shared_certified = 0;
    for (size_t i = 0; i < TC::Size(); ++i) {
        const auto* entry = TC::At(i);
        if (!(entry->families & TC::Cloth)) continue;
        ++cloth;
        // Cloth textures are placed-mesh textures: the effects' ClothMesh plus the
        // placed owner (certified or not) must both be selected.
        CHECK(entry->families & (TC::Placed | TC::CertifiedPlaced));
        const auto cloth_only = TC::Inspect(archive, entry->texture, TC::Cloth, Shader(0x17) | Shader(0x1a));
        CHECK(cloth_only.result == TC::Result::Retained);
        if (entry->families & TC::CertifiedPlaced) {
            ++cloth_shared_certified;
            const auto both = TC::Inspect(archive, entry->texture, TC::Cloth | TC::CertifiedPlaced | TC::Placed,
                                          Shader(0x17) | Shader(0x1a) | Shader(0x01));
            CHECK(both.result == TC::Result::Owned);
        }
    }
    CHECK(cloth == 14 && cloth_shared_certified == 7);
}

// The pure HleConfig -> Family map the root hands to Inspect (INTERFACES
// section 11): the control arm selects nothing, each composed key selects
// exactly its family and shader type, and keys without a texture-claiming
// owner never add a bit.
void SelectionMap() {
    const auto archive = TC::RetailArchive();
    // The control arm = every replacement selection off, stated EXPLICITLY for
    // every key (default-independent: the all-on, no-exceptions build of
    // 2026-10-09 made EVERY owned arm the compiled default
    ps2r::Config defaults;
    defaults.hle.graphics_sprites = false;   // flips 1/1b (reverted): the generated sprite arm, stated
    defaults.hle.graphics_frame = false;     // flips 2/2b (reverted): the generated frame arm, stated
    defaults.hle.graphics_strips = ps2r::GraphicsStrips::Plain;   // flip 3 (reverted): the accepted Plain arm, stated
    defaults.hle.graphics_text = false;      // the generated Font arm, stated
    defaults.hle.graphics_sky = false;       // the generated Sky arm, stated
    defaults.hle.graphics_shadows = false;   // the retained shadow application, stated
    defaults.hle.graphics_distortion = false; // the retained distortion application, stated
    defaults.hle.graphics_screen = false;    // the owned screen effects (no texture claim), stated
    defaults.hle.graphics_movie = false;     // flips 7/8b (reverted): the generated Movie picture arm, stated
    defaults.hle.movie_lifecycle = false;    // flip 4: the retired lifecycle arm, stated
    defaults.hle.movie_step = defaults.hle.movie_start = defaults.hle.movie_wait = defaults.hle.movie_stop = false;
    defaults.hle.math_helpers = false;       // flip 5: the retired generated-math arm, stated
    defaults.hle.culling = false;            // the retired culling arm, stated
    defaults.hle.stream_files = false;       // flip 6 (reverted): the STREAM/IOP file-job arm, stated
    defaults.hle.files = false;              // the retained disc-file arm, stated
    defaults.hle.sound_samples = false;      // the retained sample/bank/control arm, stated
    defaults.hle.music = false;              // flip 9b: the retired libsd/STREAM music arm, stated
    defaults.hle.music_buffers = defaults.hle.audio_mix = defaults.hle.service_time = false;
    defaults.hle.input_service = defaults.hle.save_storage = false;
    const auto none = TC::Selected(defaults.hle);
    CHECK(none.families == 0 && none.shaders == 0);
    size_t owned_by_control = 0;
    for (size_t i = 0; i < TC::Size(); ++i)
        if (TC::Inspect(archive, TC::At(i)->texture, none.families, none.shaders).result == TC::Result::Owned)
            ++owned_by_control;
    CHECK(owned_by_control == 0);
    // The compiled defaults (all-on, NO EXCEPTIONS, 2026-10-09): every hle.*
    // key is on, so the compiled default selects every texture-claiming family
    // the composed owners claim -- Sprite (flips 1/1b), Font, Sky and
    // ShadowDefault -- and their shader types; the former exceptions are now
    // on (the old value, off, is stated by `defaults` above).
    const ps2r::Config compiled_defaults;
    CHECK(compiled_defaults.hle.graphics_sprites);    // all-on, no exceptions (was off: flips 1/1b)
    CHECK(compiled_defaults.hle.graphics_frame);      // all-on, no exceptions (was off: flips 2/2b)
    CHECK(compiled_defaults.hle.graphics_strips == ps2r::GraphicsStrips::All);   // all-on, no exceptions (was plain: flip 3)
    CHECK(compiled_defaults.hle.graphics_shadows);    // all-on, no exceptions (was off: coldboot abort frame 113)
    CHECK(compiled_defaults.hle.graphics_text);       // all-on, no exceptions (was off: coldboot abort frame 119)
    CHECK(compiled_defaults.hle.stream_files);        // all-on, no exceptions (was off: flip 6, anchors refuse GCLK)
    CHECK(compiled_defaults.hle.movie_lifecycle);     // flip 4
    CHECK(compiled_defaults.hle.math_helpers);        // flip 5
    CHECK(compiled_defaults.hle.music);               // flip 9b
    CHECK(compiled_defaults.hle.graphics_movie && compiled_defaults.hle.graphics_sky &&
          compiled_defaults.hle.graphics_distortion);   // all-on
    const auto compiled = TC::Selected(compiled_defaults.hle);
    CHECK(compiled.families == (TC::Sprite | TC::Font | TC::Sky | TC::ShadowDefault));
    CHECK(compiled.shaders == (Shader(0x0a) | Shader(0x0b) | Shader(0x0d) | Shader(0x0e)));

    ps2r::Config text = defaults; text.hle.graphics_text = true;
    const auto fonts = TC::Selected(text.hle);
    CHECK(fonts.families == TC::Font && fonts.shaders == Shader(0x0d));
    ps2r::Config sprites = defaults; sprites.hle.graphics_sprites = true;
    const auto icons = TC::Selected(sprites.hle);
    CHECK(icons.families == TC::Sprite && icons.shaders == (Shader(0x0d) | Shader(0x0e)));
    ps2r::Config sky = defaults; sky.hle.graphics_sky = true;
    const auto skies = TC::Selected(sky.hle);
    CHECK(skies.families == TC::Sky && skies.shaders == Shader(0x0a));
    ps2r::Config shadows = defaults; shadows.hle.graphics_shadows = true;
    const auto shades = TC::Selected(shadows.hle);
    CHECK(shades.families == TC::ShadowDefault && shades.shaders == Shader(0x0b));
    // Texture-free selections: frame copy, movie pictures, flat strips and the
    // Movie/service keys claim no texture ID.
    ps2r::Config others = defaults;
    others.hle.graphics_frame = others.hle.graphics_movie = true;
    others.hle.graphics_strips = ps2r::GraphicsStrips::All;
    others.hle.movie_lifecycle = others.hle.files = others.hle.math_helpers = true;
    const auto nothing = TC::Selected(others.hle);
    CHECK(nothing.families == 0 && nothing.shaders == 0);
    // Unions compose bitwise and never touch the unownable bits.
    ps2r::Config all = defaults;
    all.hle.graphics_text = all.hle.graphics_sprites = all.hle.graphics_sky = all.hle.graphics_shadows = true;
    const auto both = TC::Selected(all.hle);
    CHECK(both.families == (TC::Font | TC::Sprite | TC::Sky | TC::ShadowDefault));
    CHECK((both.families & TC::Unownable) == 0);
    CHECK(both.shaders == (Shader(0x0d) | Shader(0x0e) | Shader(0x0a) | Shader(0x0b)));

    // With fonts selected every Crash page is Owned; with sprites every startup
    // HUD icon is. The Arial page is a Font-family 0x0D texture too, so the
    // consumer map says Owned under fonts: its deferral (no
    // generated Font::Read caller loads it) is TextureSelection's policy, not
    // a consumer-family fact, and must stay there.
    size_t crash = 0, hud = 0, arial = 0;
    for (const auto& owner : TO::kStartup) {
        const auto by_fonts = TC::Inspect(archive, owner.texture, fonts.families, fonts.shaders).result;
        const auto by_icons = TC::Inspect(archive, owner.texture, icons.families, icons.shaders).result;
        switch (owner.family) {
        case TO::Family::CrashFont:
            ++crash; CHECK(by_fonts == TC::Result::Owned && by_icons == TC::Result::Retained); break;
        case TO::Family::Hud:
            ++hud; CHECK(by_icons == TC::Result::Owned && by_fonts == TC::Result::Retained); break;
        case TO::Family::ArialFont:
            ++arial; CHECK(by_fonts == TC::Result::Owned && by_icons == TC::Result::Retained); break;
        }
    }
    CHECK(crash == 3 && hud == 40 && arial == 1);
    // Sky selection alone owns NO texture: every one of the 21 sky textures
    // is also an uncertified placed-mesh texture (the sky meshes are placed
    // models whose materials carry the type-0A shader), and nine of them are
    // additionally orphan-material (unownable). The Sky bit retires only with
    // the mesh's placed owner; the type-0A shader is the only one on them.
    size_t sky_owned = 0, sky_family = 0, sky_placed = 0, sky_orphan = 0;
    for (size_t i = 0; i < TC::Size(); ++i) {
        const auto* entry = TC::At(i);
        if (!(entry->families & TC::Sky)) continue;
        ++sky_family;
        CHECK(entry->shaders == Shader(0x0a));
        if (entry->families & TC::Placed) ++sky_placed;
        if (entry->families & TC::OrphanMaterial) ++sky_orphan;
        const auto ready = TC::Inspect(archive, entry->texture, skies.families, skies.shaders);
        if (ready.result == TC::Result::Owned) ++sky_owned;
        CHECK(ready.result == TC::Result::Retained && (ready.retained_families & TC::Sky) == 0);
        CHECK(ready.retained_families & TC::Placed);
        // With the placed owner also selected, the twelve non-orphan ones are Owned.
        const auto with_placed = TC::Inspect(archive, entry->texture, skies.families | TC::Placed, skies.shaders);
        CHECK((with_placed.result == TC::Result::Owned) == !(entry->families & TC::OrphanMaterial));
    }
    CHECK(sky_family == 21 && sky_owned == 0 && sky_placed == 21 && sky_orphan == 9);
    std::printf("TextureConsumers: selection map; sky textures %zu, owned by sky alone %zu, shared placed %zu, orphan %zu\n",
                sky_family, sky_owned, sky_placed, sky_orphan);
}
}  // namespace

int main() {
    try {
        Catalogue(); AgreesWithCertificate(); AgreesWithStartupOwners(); InspectSemantics(); ClothAndSharedWorld();
        SelectionMap();
        std::printf("TextureConsumers: %u checks over %zu texture IDs\n", checks, TC::Size());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
