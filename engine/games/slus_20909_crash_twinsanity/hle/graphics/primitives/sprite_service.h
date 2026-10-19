#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_intent.h"

namespace title {

class SpriteService {
public:
    using Body = TextureBindings::Body;
    using Capture = TextureBindings::Capture;
    using Installer = TextureBindings::Installer;
    struct Hooks {
        bool enabled = false;
        PrimitiveMaterial::Read read;
        std::function<bool(MaterialState::Environment&)> environment;
        // Pure per-object eligibility. Every texture consumer must agree with
        // TextureBindings::Select. False is counted and uses the original body.
        std::function<bool(uint32_t sprite)> selected;
        // Root owns exact bucket/predecessor and target leases. True records
        // the complete draw plus required upload with its retained texture
        // version. False records nothing: the call REFUSES to the original.
        // Empty intent still requires root target validation; true must then
        // record no upload/draw and earn no backend completion credit.
        std::function<bool(const SpriteIntent::Output&)> record;
        // Attribution of refusals (counted, original runs) and faults (thrown).
        // Detail codes: kInterior/kEnvironment/kRecordRefused below, or
        // kCapture | SpriteIntent::Result for a capture result.
        GraphicsRefusals::Sink refuse, fault;
    };
    static constexpr uint32_t kInterior = 1, kEnvironment = 2, kRecordRefused = 3, kSelection = 4,
                              kCapture = 0x100;
    // refused = selected calls returned to the original before any commit
    // (nothing recorded); failed = faults (selection throw, record throw).
    struct Counters { uint64_t calls = 0, replaced = 0, model_calls = 0, failed = 0, empty = 0, refused = 0; };
    SpriteService(TextureBindings& textures, Hooks hooks);
    // Captures all four original bodies before installing any. False may
    // leave an installed prefix; abort startup. Lifetime >= dispatch table.
    bool Install(const Capture& capture, const Installer& install);
    Counters Counts() const { return counts_; }

private:
    TextureBindings& textures_;
    Hooks hooks_;
    Counters counts_;
    bool installed_ = false, faulted_ = false;
};

}  // namespace title
