#pragma once

#include <string>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_loads.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_pass.h"

namespace title {

// Game-owner composition. Stable dispatch entries survive a checkpoint restore;
// conversion and source grouping are replaced together, after a complete fence.
class ShadowLibrary {
public:
    using Body = ShadowPass::Body;
    using Capture = ShadowPass::Capture;
    using Installer = ShadowPass::Installer;
    struct Snapshot {
        bool selected = false;
        ShadowAssets::Snapshot assets;
    };
    struct Counters {
        ShadowLoads::Counters loads;
        ShadowPass::Counters passes;
        ShadowAssets::Counters assets;
        uint64_t restores = 0;
    };
    explicit ShadowLibrary(ShadowPass::Hooks);
    ~ShadowLibrary();
    bool Install(const Capture&, const Installer&);
    // `unlinked_recording_open`: see ShadowPass::CanCheckpoint (the capture-time settle).
    bool CanCheckpoint(std::string* error = nullptr, bool unlinked_recording_open = false) const;
    bool CaptureState(Snapshot&, std::string* error = nullptr, bool unlinked_recording_open = false) const;
    // Pure incoming-source validation, followed by staging a separate owner.
    // No RAM writes, render work, live dispatch change or missing-state reset.
    bool Stage(const Snapshot&, const MaterialState::Read& incoming, std::string* error = nullptr);
    void DiscardStaged();
    // Root has already restored RAM and fenced output. A staged state must
    // still match the actual incoming RAM before publication.
    bool Publish(std::string* error = nullptr);
    Counters Counts() const;
    bool Selected() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
