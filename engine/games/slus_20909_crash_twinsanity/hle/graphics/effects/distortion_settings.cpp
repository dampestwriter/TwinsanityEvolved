#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_settings.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace title {
DistortionSettings::DistortionSettings(Read read) : read_(std::move(read)) {}
bool DistortionSettings::Install(const Capture& capture, const Installer& install) {
    if (installed_) return true;
    if (!capture || !install || !read_) return false;
    auto original = capture(kCall);
    if (!original) return false;
    original_ = original;
    const auto active = std::make_shared<std::atomic<bool>>(false);
    bool accepted = false;
    try {
        accepted = install(kCall, [this, active, original](CpuContext& c, uint32_t e) {
            // A rejecting installer may still have published this callback;
            // until the install commits it only forwards.
            if (!active->load(std::memory_order_acquire)) { original(c, e); return; }
            Run(c, e);
        });
    } catch (...) { original_ = {}; throw; }
    if (!accepted) { original_ = {}; return false; }
    active->store(true, std::memory_order_release);
    installed_ = true;
    return true;
}
void DistortionSettings::Run(CpuContext& context, uint32_t entry) {
    // The settings body is pure CPU allocation and cannot yield; a resumed
    // interior has no proved shader argument and would publish a false version.
    if (entry && entry != kCall) {
        ++counts_.interior;
        throw std::runtime_error("DistortionSettings needs a complete source settings call");
    }
    const uint32_t shader = context.cpuRegs.GPR.r[4].UL[0];
    ++counts_.calls;
    // Capture at the call: the constructor's later linear/K/L writes
    // (TR materials.cpp MakeDistortionMaterial) never reached the settings.
    DistortionMaterial::Settings settings;
    const auto result = DistortionMaterial::CaptureSettings(read_, shader, settings);
    ++state_.generation;
    if (result == DistortionMaterial::Result::Ok) {
        state_.shader = shader;
        state_.settings = settings;
        ++counts_.captured;
    } else {
        // Not the retail type18 object: no version is current until the next
        // accepted call. A selected pass refuses the material at its first draw.
        state_.shader = 0;
        state_.settings = {};
        ++counts_.rejected;
    }
    ++active_;
    try { original_(context, entry); --active_; }
    catch (...) { --active_; throw; }
}
bool DistortionSettings::Current(uint32_t shader, DistortionMaterial::Settings& out,
                                 uint64_t* generation) const {
    if (!shader || state_.shader != shader) return false;
    out = state_.settings;
    if (generation) *generation = state_.generation;
    return true;
}
bool DistortionSettings::CaptureState(Snapshot& out) const {
    if (!CanCheckpoint()) return false;
    out = state_;
    return true;
}
bool DistortionSettings::Validate(const Snapshot& snapshot) {
    if (snapshot.version != 1) return false;
    if (!snapshot.shader) return true;
    const auto segment = snapshot.shader & 0xe0000000, at = snapshot.shader & 0x1fffffff;
    if ((segment != 0 && segment != 0x80000000 && segment != 0xa0000000) || (snapshot.shader & 3) ||
        !at || at > 0x2000000 - 0x70) return false;
    return snapshot.generation != 0 && DistortionMaterial::Supported(snapshot.settings);
}
bool DistortionSettings::Restore(const Snapshot& snapshot) {
    if (!CanCheckpoint() || !Validate(snapshot)) return false;
    state_ = snapshot;
    return true;
}
}  // namespace title
