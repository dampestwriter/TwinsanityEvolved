#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_backend.h"

#include <stdexcept>

void TextureTestBackend::UploadLevel(ps2r::render::SurfaceId id, int level, int width, int height, const void* rgba) {
    if (fail_mip) throw std::runtime_error("injected mip upload failure");
    const auto* bytes = static_cast<const uint8_t*>(rgba);
    mip_bytes[{id.id, level}] = {bytes, bytes + size_t(width) * height * 4};
    FakeBackend::UploadLevel(id, level, width, height, rgba);
}
void TextureTestBackend::FinishMipChain(ps2r::render::SurfaceId id, int levels, bool synthesize) {
    synthesized = synthesize;
    FakeBackend::FinishMipChain(id, levels, synthesize);
}
