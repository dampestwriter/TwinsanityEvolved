#pragma once

#include "tests/lib/fake_backend.h"

class TextureTestBackend : public ps2r::test::FakeBackend {
public:
    std::map<std::pair<uint32_t, int>, std::vector<uint8_t>> mip_bytes;
    bool fail_mip = false, synthesized = false;
    void UploadLevel(ps2r::render::SurfaceId id, int level, int width, int height, const void* rgba) override;
    void FinishMipChain(ps2r::render::SurfaceId id, int levels, bool synthesize) override;
};
