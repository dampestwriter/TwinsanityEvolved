#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_backend.h"

#include <stdexcept>

ps2r::render::DrawResult EffectsBackend::Draw(const ps2r::gs::DrawIr& draw,
    const ps2r::gs::TextureSource& source, const ps2r::render::DrawParams& parameters) {
    if (fail_after_draws >= 0 && draws >= fail_after_draws)
        throw std::runtime_error("injected backend draw failure");
    return FakeBackend::Draw(draw, source, parameters);
}
