#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_service.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace title {
SpriteService::SpriteService(TextureBindings& textures, Hooks hooks)
    : textures_(textures), hooks_(std::move(hooks)) {}
bool SpriteService::Install(const Capture& capture, const Installer& install) {
    if (installed_ || !capture || !install ||
        (hooks_.enabled && (!hooks_.read || !hooks_.environment || !hooks_.selected || !hooks_.record)))
        return false;
    constexpr std::array<uint32_t, 4> entries{
        SpriteIntent::kPlain, SpriteIntent::kColoured, SpriteIntent::kPlaced, SpriteIntent::kPlacedColoured};
    std::array<Body, 4> originals;
    for (size_t i = 0; i < entries.size(); ++i) {
        originals[i] = capture(entries[i]);
        if (!originals[i]) return false;
    }
    installed_ = true;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto function = entries[i];
        if (!install(function, [this, function, original = originals[i]](CpuContext& c, uint32_t entry) {
            ++counts_.calls;
            if (faulted_) throw std::runtime_error("SpriteService: faulted producer cannot replay");
            if (!hooks_.enabled) { ++counts_.model_calls; original(c, entry); return; }
            // Nothing below commits until hooks_.record returns true: every
            // early exit is a counted refusal and the original body draws.
            const auto refuse = [&](GraphicsRefusals::Reason reason, uint32_t detail) {
                ++counts_.refused;
                ++counts_.model_calls;
                if (hooks_.refuse) hooks_.refuse(reason, function, detail);
                original(c, entry);
            };
            if (entry && entry != function) { refuse(GraphicsRefusals::Reason::Ordering, kInterior); return; }
            const auto arg = [&](unsigned n) { return c.cpuRegs.GPR.r[4 + n].UL[0]; };
            bool selected = false;
            try { selected = hooks_.selected(arg(0)); }
            catch (...) {
                // An owned texture reached by a consumer no arm can draw.
                ++counts_.failed; faulted_ = true;
                if (hooks_.fault) hooks_.fault(GraphicsRefusals::Reason::UnsupportedState, function, kSelection);
                throw;
            }
            if (!selected) { ++counts_.model_calls; original(c, entry); return; }
            MaterialState::Environment environment;
            if (!hooks_.environment(environment)) { refuse(GraphicsRefusals::Reason::Ordering, kEnvironment); return; }
            SpriteIntent::Output output;
            const auto result = SpriteIntent::Capture(hooks_.read, textures_, function, arg(0), arg(1), arg(2),
                                                      environment, output);
            if (result != SpriteIntent::Result::Ok && result != SpriteIntent::Result::Empty) {
                refuse(GraphicsRefusals::Reason::UnsupportedState, kCapture | uint32_t(result));
                return;
            }
            bool recorded = false;
            try { recorded = hooks_.record(output); }
            catch (...) {
                ++counts_.failed; faulted_ = true;
                if (hooks_.fault) hooks_.fault(GraphicsRefusals::Reason::Ordering, function, kRecordRefused);
                throw;
            }
            if (!recorded) { refuse(GraphicsRefusals::Reason::Ordering, kRecordRefused); return; }
            // Empty: material, texture generation, source frame and all used
            // inputs were validated; the root recorded no upload/draw and the
            // retained material caches stay as authored by their own producer.
            ++counts_.replaced;
            if (result == SpriteIntent::Result::Empty) ++counts_.empty;
        })) { faulted_ = true; return false; }
    }
    return true;
}
}  // namespace title
