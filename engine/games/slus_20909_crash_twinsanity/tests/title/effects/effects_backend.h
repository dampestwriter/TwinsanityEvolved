#pragma once

#include "tests/lib/fake_backend.h"

class EffectsBackend : public ps2r::test::FakeBackend {
public:
    int fail_after_draws = -1;
    ps2r::render::DrawResult Draw(const ps2r::gs::DrawIr&, const ps2r::gs::TextureSource&,
                                  const ps2r::render::DrawParams&) override;
};
