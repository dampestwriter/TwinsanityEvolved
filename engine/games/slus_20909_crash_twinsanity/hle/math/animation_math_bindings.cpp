#include "games/slus_20909_crash_twinsanity/hle/math/animation_math.h"

#include <utility>

#include "engine/ee/cpu/dispatch_table.h"

namespace title {

bool AnimationMath::Install(const Capture& capture, const Installer& installer, bool replace_helpers) {
    if (installation_ || !capture || !installer) return false;
    const Body captured = capture(kReadJointAngleAddress);
    if (!captured) return false;
    const Body original = DispatchTable::CountCalls(kReadJointAngleAddress, captured, generated_body_calls_);
    const auto active = std::make_shared<std::atomic<bool>>(false);
    Body body = original;
    if (replace_helpers) {
        Body entry_body = [this, active, original](CpuContext& context, uint32_t entry) {
            if (!active->load(std::memory_order_acquire)) original(context, entry);
            else if (!CanReplace(context)) {
                unsupported_.fetch_add(1, std::memory_order_relaxed);
                original(context, entry);
            } else ReadJointAngle(context);
        };
        body = DispatchTable::DelegateInterior(kReadJointAngleAddress, original, std::move(entry_body));
    }
    body = DispatchTable::CountCalls(kReadJointAngleAddress, std::move(body), continuations_,
        DispatchTable::CountMode::InteriorCalls, active);
    const auto restore = [&] {
        try { (void)installer(kReadJointAngleAddress, captured); }
        catch (...) {}  // Retained callbacks still forward without an active owner.
    };
    try {
        if (installer(kReadJointAngleAddress, std::move(body))) {
            installation_ = active;
            active->store(true, std::memory_order_release);
            return true;
        }
    } catch (...) {
        restore();
        throw;
    }
    restore();
    return false;
}

}  // namespace title
