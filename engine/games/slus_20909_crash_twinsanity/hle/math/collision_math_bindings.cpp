#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"

#include <utility>

#include "engine/ee/cpu/dispatch_table.h"

namespace title {

bool CollisionMath::Install(const Capture& capture, const Installer& installer, bool replace_helpers) {
    if (installation_ || !capture || !installer) return false;
    using Binding = std::pair<uint32_t, void (CollisionMath::*)(CpuContext&)>;
    const std::array<Binding, 8> bindings{{
        {kStartEdgesAddress, &CollisionMath::StartTriangleEdgeTests},
        {kFinishEdgesAddress, &CollisionMath::FinishTriangleEdgeTests},
        {kVertexDifferencesAddress, &CollisionMath::MakeVertexDifferences},
        {kGroupPointsAddress, &CollisionMath::GroupPoints},
        {kGroupsRangeAddress, &CollisionMath::GroupsRange},
        {kSixteenGroupsRangeAddress, &CollisionMath::SixteenGroupsRange},
        {kLoadSupportAddress, &CollisionMath::LoadTriangleHullSupport},
        {kSupportRangeAddress, &CollisionMath::TriangleHullSupportRange},
    }};
    std::array<Body, bindings.size()> captured, originals;
    for (size_t i = 0; i < bindings.size(); ++i) {
        captured[i] = capture(bindings[i].first);
        if (!captured[i]) return false;
        originals[i] = DispatchTable::CountCalls(bindings[i].first, captured[i],
            std::shared_ptr<std::atomic<uint64_t>>(generated_body_calls_, &(*generated_body_calls_)[i]));
    }
    const auto active = std::make_shared<std::atomic<bool>>(false);
    const auto rollback = [&](size_t last) {
        for (size_t remaining = last + 1; remaining != 0; --remaining) {
            const size_t index = remaining - 1;
            try { (void)installer(bindings[index].first, captured[index]); }
            catch (...) {}  // An unpublished owner leaves retained callbacks inactive.
        }
    };
    for (size_t i = 0; i < bindings.size(); ++i) {
        try {
            const auto binding = bindings[i];
            Body body = originals[i];
            if (replace_helpers) {
                Body entry_body = [this, binding, original = originals[i], active]
                    (CpuContext& context, uint32_t entry) {
                    if (!active->load(std::memory_order_acquire)) original(context, entry);
                    else if (!CanReplace(binding.first, context)) {
                        unsupported_.fetch_add(1, std::memory_order_relaxed);
                        original(context, entry);
                    }
                    else (this->*binding.second)(context);
                };
                body = DispatchTable::DelegateInterior(binding.first, originals[i], std::move(entry_body));
            }
            body = DispatchTable::CountCalls(binding.first, std::move(body), continuations_,
                DispatchTable::CountMode::InteriorCalls, active);
            if (installer(binding.first, std::move(body))) continue;
        } catch (...) {
            rollback(i);
            throw;
        }
        rollback(i);
        return false;
    }
    installation_ = active;
    active->store(true, std::memory_order_release);
    return true;
}

}  // namespace title
