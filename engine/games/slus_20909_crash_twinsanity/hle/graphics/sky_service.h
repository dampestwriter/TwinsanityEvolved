#pragma once

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_intent.h"

namespace title {

// Optional complete source boundary. The root supplies ordered recording and
// source-owned named resources. No default change and no model-state snapshot.
class SkyService {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    struct Hooks {
        bool enabled = false; // one-line model/host selection before live gates
        MaterialState::Read read;
        std::function<bool(MaterialState::Environment&)> environment;
        // True owns the complete draw and its dependencies at this bucket-0
        // source position. False MUST leave recording unchanged. An exception
        // is fatal to this submission and never triggers generated fallback.
        // Require the named SkyColor content and scene attachments to be ready.
        std::function<bool(const SkyIntent::Output&)> record;
    };
    struct Counters {
        uint64_t calls = 0, replaced = 0, model_calls = 0, interior_calls = 0;
        uint64_t recording_failures = 0;
    };
    explicit SkyService(Hooks hooks);
    // Install BEFORE FrameEnvironment, which must wrap this boundary so its
    // successful-completion observation runs in either selected arm.
    // A false installer must leave dispatch unchanged. No other entry changes.
    bool Install(const Capture& capture, const Installer& installer);
    void Run(CpuContext& context, uint32_t entry);
    Counters Counts() const;

private:
    Hooks hooks_;
    Body original_;
    Counters counts_;
    bool installed_ = false, faulted_ = false;
};

}  // namespace title
