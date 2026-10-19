#include "games/slus_20909_crash_twinsanity/hle/math/joint_library.h"

#include <stdexcept>
#include <utility>

#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/cpu/guest_park.h"

namespace title {
class JointLibrary::CountsState {
public:
    std::atomic<uint64_t> rotations{0}, generated{0}, interiors{0}, inputs{0};
    std::atomic<uint64_t> transforms{0}, matrices{0}, done{0}, generated_transform{0};
    std::atomic<uint64_t> animations{0}, blended{0}, angle_reads{0}, generated_animate{0};
    std::atomic<uint64_t> refused_rotate{0}, refused_transform{0}, refused_animate{0}, faults{0};
    std::array<std::atomic<uint64_t>, 5> reasons{};
    // A declined entry: counted per entry and per reason, then the captured
    // original runs on the unchanged entry context (its own counter moves).
    void Refuse(std::atomic<uint64_t>& entry, Refusal refusal) {
        inputs.fetch_add(1, std::memory_order_relaxed);
        entry.fetch_add(1, std::memory_order_relaxed);
        reasons[static_cast<size_t>(refusal)].fetch_add(1, std::memory_order_relaxed);
    }
};
class JointLibrary::Installation {
public:
    std::atomic<JointLibrary*> owner{nullptr};
};

JointLibrary::~JointLibrary() {
    if (installation_) installation_->owner.store(nullptr, std::memory_order_release);
}

bool JointLibrary::Install(const CaptureBody& capture, const Installer& installer, bool selected) {
    if (installation_ || !capture || !installer) return false;
    constexpr std::array<uint32_t, 3> entries{kRotate, kTransform, kAnimate};
    std::array<Body, 3> captured;
    for (size_t i = 0; i < entries.size(); ++i) {
        captured[i] = capture(entries[i]);
        if (!captured[i]) return false;
    }
    if (!counts_) counts_ = std::make_shared<CountsState>();
    const auto counts = counts_;
    const auto installation = std::make_shared<Installation>();
    // The angle decoder stays its own counted boundary: Animate reaches it
    // through the live dispatch surface (or the caller's stand-in), never by
    // binding its body here. Each delegation is counted at the dispatch.
    const Call forward = call_ ? call_ : Call{[](CpuContext& context, uint32_t address) {
        DispatchTable::Invoke(address, context);
    }};
    call_ = [counts, forward](CpuContext& context, uint32_t address) {
        counts->angle_reads.fetch_add(1, std::memory_order_relaxed);
        forward(context, address);
    };
    const auto rotate_original = DispatchTable::CountCalls(kRotate, captured[0],
        std::shared_ptr<std::atomic<uint64_t>>(counts, &counts->generated));
    const auto transform_original = DispatchTable::CountCalls(kTransform, captured[1],
        std::shared_ptr<std::atomic<uint64_t>>(counts, &counts->generated_transform));
    const auto animate_original = DispatchTable::CountCalls(kAnimate, captured[2],
        std::shared_ptr<std::atomic<uint64_t>>(counts, &counts->generated_animate));
    std::array<Body, 3> bodies{rotate_original, transform_original, animate_original};
    if (selected) {
        const auto interior = [&](uint32_t address, const Body& original) {
            return DispatchTable::CountCalls(address, original,
                std::shared_ptr<std::atomic<uint64_t>>(counts, &counts->interiors));
        };
        Body rotate = [installation, counts, original = rotate_original](CpuContext& context, uint32_t address) {
            auto* owner = installation->owner.load(std::memory_order_acquire);
            if (!owner) { original(context, address); return; }
            if (!owner->Rotate(context)) {
                counts->Refuse(counts->refused_rotate, owner->LastRefusal());
                original(context, address);
                return;
            }
            counts->rotations.fetch_add(1, std::memory_order_relaxed);
        };
        Body transform = [installation, counts, original = transform_original](CpuContext& context, uint32_t address) {
            auto* owner = installation->owner.load(std::memory_order_acquire);
            if (!owner) { original(context, address); return; }
            const auto result = owner->Transform(context);
            if (result == TransformResult::Invalid) {
                counts->Refuse(counts->refused_transform, owner->LastRefusal());
                original(context, address);
                return;
            }
            counts->transforms.fetch_add(1, std::memory_order_relaxed);
            (result == TransformResult::Written ? counts->matrices : counts->done)
                .fetch_add(1, std::memory_order_relaxed);
        };
        Body animate = [installation, counts, original = animate_original](CpuContext& context, uint32_t address) {
            auto* owner = installation->owner.load(std::memory_order_acquire);
            if (!owner) { original(context, address); return; }
            AnimateResult result;
            try { result = owner->Animate(context); }
            catch (const std::runtime_error& fault) {
                // A source object left unmapped after publication began: the
                // frame is already mutated, so no arm can continue this call.
                // The leg parks visibly with the reason; the process never aborts.
                counts->faults.fetch_add(1, std::memory_order_relaxed);
                GuestPark::Forever("joint_library_fault", kAnimate, fault.what());
            }
            if (result == AnimateResult::Invalid) {
                counts->Refuse(counts->refused_animate, owner->LastRefusal());
                original(context, address);
                return;
            }
            counts->animations.fetch_add(1, std::memory_order_relaxed);
            if (result == AnimateResult::Blended) counts->blended.fetch_add(1, std::memory_order_relaxed);
        };
        bodies[0] = DispatchTable::DelegateInterior(kRotate, interior(kRotate, rotate_original), std::move(rotate));
        bodies[1] = DispatchTable::DelegateInterior(kTransform, interior(kTransform, transform_original),
                                                    std::move(transform));
        bodies[2] = DispatchTable::DelegateInterior(kAnimate, interior(kAnimate, animate_original),
                                                    std::move(animate));
    }
    const auto rollback = [&](size_t last) {
        for (size_t remaining = last + 1; remaining != 0; --remaining) {
            try { (void)installer(entries[remaining - 1], captured[remaining - 1]); }
            catch (...) {}  // An unpublished owner leaves retained callbacks inactive.
        }
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        bool installed;
        try { installed = installer(entries[i], std::move(bodies[i])); }
        catch (...) { rollback(i); throw; }
        if (!installed) { rollback(i); return false; }
    }
    installation_ = installation;
    installation->owner.store(this, std::memory_order_release);
    return true;
}

JointLibrary::Counters JointLibrary::Counts() const {
    if (!counts_) return {};
    const auto load = [](const std::atomic<uint64_t>& value) { return value.load(std::memory_order_relaxed); };
    const auto& c = *counts_;
    return {load(c.rotations), load(c.generated), load(c.interiors), load(c.inputs),
            load(c.transforms), load(c.matrices), load(c.done), load(c.generated_transform),
            load(c.animations), load(c.blended), load(c.angle_reads), load(c.generated_animate),
            load(c.refused_rotate), load(c.refused_transform), load(c.refused_animate),
            load(c.reasons[static_cast<size_t>(Refusal::State)]), load(c.reasons[static_cast<size_t>(Refusal::Frame)]),
            load(c.reasons[static_cast<size_t>(Refusal::Objects)]),
            load(c.reasons[static_cast<size_t>(Refusal::Channels)]), load(c.faults)};
}

std::string JointLibrary::Census() const {
    const auto counts = Counts();
    return "rotations=" + std::to_string(counts.rotations) +
        ",generated_body_rotate=" + std::to_string(counts.generated_body_rotate) +
        ",generated_interiors=" + std::to_string(counts.generated_interiors) +
        ",unsupported_inputs=" + std::to_string(counts.unsupported_inputs) +
        ",transforms=" + std::to_string(counts.transforms) +
        ",matrices_written=" + std::to_string(counts.matrices_written) +
        ",done_returns=" + std::to_string(counts.done_returns) +
        ",generated_body_transform=" + std::to_string(counts.generated_body_transform) +
        ",animations=" + std::to_string(counts.animations) +
        ",blended_animations=" + std::to_string(counts.blended_animations) +
        ",angle_reads=" + std::to_string(counts.angle_reads) +
        ",generated_body_animate=" + std::to_string(counts.generated_body_animate) +
        ",refused_rotate=" + std::to_string(counts.refused_rotate) +
        ",refused_transform=" + std::to_string(counts.refused_transform) +
        ",refused_animate=" + std::to_string(counts.refused_animate) +
        ",refusal_state=" + std::to_string(counts.refusal_state) +
        ",refusal_frame=" + std::to_string(counts.refusal_frame) +
        ",refusal_objects=" + std::to_string(counts.refusal_objects) +
        ",refusal_channels=" + std::to_string(counts.refusal_channels) +
        ",faults=" + std::to_string(counts.faults);
}

}  // namespace title
