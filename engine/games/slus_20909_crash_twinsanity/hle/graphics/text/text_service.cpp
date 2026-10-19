#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_service.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace title {
TextService::TextService(TextureBindings& textures, Hooks hooks)
    : textures_(textures), hooks_(std::move(hooks)) {}
bool TextService::Install(const Capture& capture, const Installer& install) {
    if (installed_ || !capture || !install ||
        (hooks_.enabled && (!hooks_.read || !hooks_.environment || !hooks_.selected || !hooks_.record)))
        return false;
    constexpr std::array<uint32_t, 4> entries{
        TextIntent::kDrawFontTexts, TextIntent::kBegin, TextIntent::kDraw, TextIntent::kEnd};
    std::array<Body, 4> originals;
    for (size_t n = 0; n < entries.size(); ++n) {
        originals[n] = capture(entries[n]);
        if (!originals[n]) return false;
    }
    installed_ = true;
    for (size_t n = 0; n < entries.size(); ++n) {
        const uint32_t function = entries[n];
        if (!install(function, [this, function, original = originals[n]](CpuContext& c, uint32_t entry) {
            ++counts_.calls;
            if (faulted_) throw std::runtime_error("TextService: faulted producer cannot replay");
            if (!hooks_.enabled) { ++counts_.model_calls; original(c, entry); return; }
            // Nothing below commits until hooks_.record returns true (the root
            // writes the material list only on acceptance): every early exit
            // is a counted refusal and the original body runs.
            const auto refuse = [&](GraphicsRefusals::Reason reason, uint32_t detail) {
                ++counts_.refused;
                ++counts_.model_calls;
                if (hooks_.refuse) hooks_.refuse(reason, function, detail);
                original(c, entry);
            };
            if (entry && entry != function) { refuse(GraphicsRefusals::Reason::Ordering, kInterior); return; }
            uint32_t font = c.cpuRegs.GPR.r[4].UL[0];
            const uint32_t queue = c.cpuRegs.GPR.r[5].UL[0];
            if (function == TextIntent::kDrawFontTexts) {
                std::array<uint8_t, 12> bytes{};
                const uint32_t at = queue & 0x1fffffff, segment = queue & 0xe0000000;
                // RAM alias or the scratchpad window (TR entry.cpp:83), as TextIntent.
                const bool scratchpad = (queue & 0xffffc000) == 0x70000000 &&
                    bytes.size() <= 0x4000 - (queue & 0x3fff);
                const bool ram = at && at <= 0x2000000 - bytes.size() &&
                    (segment == 0 || segment == 0x80000000 || segment == 0xa0000000);
                if ((queue & 3) || !(scratchpad || ram) ||
                    !hooks_.read(queue, bytes.data(), bytes.size())) {
                    refuse(GraphicsRefusals::Reason::UnsupportedState, kInvalidQueue);
                    return;
                }
                const auto word = [&](size_t offset) {
                    const auto* p = bytes.data() + offset;
                    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
                };
                if (!word(8)) { ++counts_.empty_queues; return; }
                font = word(0);
            }
            bool selected = false;
            try { selected = hooks_.selected(font); }
            catch (...) {
                // An owned page reached by a font no arm can draw.
                ++counts_.failed; faulted_ = true;
                if (hooks_.fault) hooks_.fault(GraphicsRefusals::Reason::UnsupportedState, function, kSelection);
                throw;
            }
            if (!selected) { ++counts_.model_calls; original(c, entry); return; }
            if (function != TextIntent::kDrawFontTexts) {
                refuse(GraphicsRefusals::Reason::Ordering, kLeafWithSelectedFont);
                return;
            }
            MaterialState::Environment environment;
            if (!hooks_.environment(environment)) { refuse(GraphicsRefusals::Reason::Ordering, kEnvironment); return; }
            TextIntent::Output output;
            const auto result = TextIntent::Capture(hooks_.read, textures_, queue, environment, output);
            if (result != TextIntent::Result::Ok) {
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
            ++counts_.replaced;
        })) { faulted_ = true; return false; }
    }
    return true;
}
}  // namespace title
