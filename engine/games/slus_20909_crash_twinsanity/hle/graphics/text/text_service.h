#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_intent.h"

namespace title {

class TextService {
public:
    using Body = TextureBindings::Body;
    using Capture = TextureBindings::Capture;
    using Installer = TextureBindings::Installer;
    struct Hooks {
        bool enabled = false;
        PrimitiveMaterial::Read read;
        std::function<bool(MaterialState::Environment&)> environment;
        std::function<bool(uint32_t font)> selected;
        // Complete source material batches, in first-touch order. Root groups
        // subsequent batches of the same material until source FlushMaterials.
        // False accepts nothing: the call REFUSES to the original. A partial
        // recording failure must throw (a fault).
        std::function<bool(const TextIntent::Output&)> record;
        // Attribution of refusals (counted, original runs) and faults (thrown).
        // Detail codes: kInterior/kInvalidQueue/kLeafWithSelectedFont/
        // kEnvironment/kRecordRefused, or kCapture | TextIntent::Result.
        GraphicsRefusals::Sink refuse, fault;
    };
    static constexpr uint32_t kInterior = 1, kInvalidQueue = 2, kLeafWithSelectedFont = 3,
                              kEnvironment = 4, kRecordRefused = 5, kSelection = 6, kCapture = 0x100;
    // refused = selected calls returned to the original before any commit
    // (no guest list word written, nothing recorded); failed = faults.
    struct Counters {
        uint64_t calls = 0, replaced = 0, empty_queues = 0, model_calls = 0, failed = 0, refused = 0;
    };
    TextService(TextureBindings& textures, Hooks hooks);
    // Whole DrawFontTexts replaces Begin/Draw/End. A selected font reaching
    // one of the three leaf bindings directly is an ordering refusal: the
    // original leaf runs and is counted; the owned arm never batches packets.
    // All originals captured first; failed installation aborts startup.
    bool Install(const Capture&, const Installer&);
    Counters Counts() const { return counts_; }

private:
    TextureBindings& textures_;
    Hooks hooks_;
    Counters counts_;
    bool installed_ = false, faulted_ = false;
};

}  // namespace title
