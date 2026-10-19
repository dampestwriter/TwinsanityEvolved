#pragma once

#include <memory>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_pass.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_settings.h"

namespace title {

// The complete bucket-23 distortion owner: block-list observers, the settings
// version owner and the producer, installed together and checkpointed as one
// value state. Unselected it installs only the producer's model arm (every
// original call forwarded and counted); nothing of the hardware model is
// deepened or replayed on either arm.
class DistortionLibrary {
public:
    using Body = DistortionPass::Body;
    using Capture = DistortionPass::Capture;
    using Installer = DistortionPass::Installer;
    struct Snapshot {
        bool selected = false;
        DistortionBlocks::Snapshot blocks;
        DistortionSettings::Snapshot settings;
    };
    struct Counters {
        DistortionBlocks::Counters blocks;
        DistortionSettings::Counters settings;
        DistortionPass::Counters pass;
        uint64_t restores = 0;
    };
    // hooks.settings is supplied by the library's own settings owner.
    explicit DistortionLibrary(DistortionPass::Hooks);
    ~DistortionLibrary();
    bool Install(const Capture&, const Installer&);
    // `unlinked_recording_open`: see DistortionPass::CanCheckpoint (the capture-time settle).
    bool CanCheckpoint(std::string* error = nullptr, bool unlinked_recording_open = false) const;
    bool CaptureState(Snapshot&, std::string* error = nullptr, bool unlinked_recording_open = false) const;
    // Pure incoming validation followed by staging a separate owner: no RAM
    // writes, render work, live dispatch change or missing-state reset.
    bool Stage(const Snapshot&, const MaterialState::Read& incoming, std::string* error = nullptr);
    void DiscardStaged();
    // Root has restored RAM and fenced output; the staged owner becomes live.
    bool Publish(std::string* error = nullptr);
    // Abandoned frame after the root output fence (DistortionPass semantics).
    void ResetAfterQuiescence();
    Counters Counts() const;
    bool Selected() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
