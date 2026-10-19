#include "games/slus_20909_crash_twinsanity/hle/math/clipping_math.h"

#include <stdexcept>
#include <utility>

#include "engine/ee/cpu/dispatch_table.h"

namespace title {
class ClippingMath::CountsState {
public:
    std::array<std::atomic<uint64_t>, 2> generated{};
    std::atomic<uint64_t> box_at{0}, box{0}, corners{0}, entries{0}, inputs{0};
};
class ClippingMath::Installation {
public:
    std::atomic<ClippingMath*> owner{nullptr};
};

ClippingMath::~ClippingMath() {
    if (installation_) installation_->owner.store(nullptr, std::memory_order_release);
}

bool ClippingMath::Install(const CaptureBody& capture, const Installer& installer, bool selected) {
    if (installation_ || !capture || !installer) return false;
    const auto& entries = Entries();
    std::array<Body, 2> originals;
    for (size_t i = 0; i < entries.size(); ++i) {
        originals[i] = capture(entries[i].address);
        if (!originals[i]) return false;
    }
    if (!counts_) counts_ = std::make_shared<CountsState>();
    auto counts = counts_;
    auto installation = std::make_shared<Installation>();
    const auto rollback = [&](size_t last) {
        for (size_t n = last + 1; n != 0; --n) {
            try { (void)installer(entries[n - 1].address, originals[n - 1]); }
            catch (...) {}
        }
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto entry = entries[i];
        const auto original = DispatchTable::CountCalls(entry.address, originals[i],
            std::shared_ptr<std::atomic<uint64_t>>(counts, &counts->generated[i]));
        Body body = original;
        if (selected) body = [installation, counts, original, entry](CpuContext& context, uint32_t address) {
            auto* owner = installation->owner.load(std::memory_order_acquire);
            if (!owner) { original(context, address); return; }
            const auto source = address ? address : entry.address;
            if (source != entry.address && !(entry.address == 0x20157c && source == kCorners)) {
                counts->entries.fetch_add(1, std::memory_order_relaxed);
                throw std::runtime_error("ClippingMath: unsupported source entry");
            }
            if (!owner->Invoke(source, context)) {
                counts->inputs.fetch_add(1, std::memory_order_relaxed);
                throw std::runtime_error("ClippingMath: invalid source object span");
            }
            if (source == 0x2014d8) counts->box_at.fetch_add(1, std::memory_order_relaxed);
            if (source == 0x20157c) counts->box.fetch_add(1, std::memory_order_relaxed);
            // Every complete operation computed one actual eight-corner batch.
            counts->corners.fetch_add(1, std::memory_order_relaxed);
        };
        bool installed;
        try { installed = installer(entry.address, std::move(body)); }
        catch (...) { rollback(i); throw; }
        if (!installed) { rollback(i); return false; }
    }
    installation_ = installation;
    installation->owner.store(this, std::memory_order_release);
    return true;
}

ClippingMath::Counters ClippingMath::Counts() const {
    Counters result{};
    if (!counts_) return result;
    result.box_at = counts_->box_at.load(std::memory_order_relaxed);
    result.box = counts_->box.load(std::memory_order_relaxed);
    result.corners = counts_->corners.load(std::memory_order_relaxed);
    for (size_t i = 0; i < result.generated_body_calls.size(); ++i)
        result.generated_body_calls[i] = counts_->generated[i].load(std::memory_order_relaxed);
    result.unsupported_entries = counts_->entries.load(std::memory_order_relaxed);
    result.unsupported_inputs = counts_->inputs.load(std::memory_order_relaxed);
    return result;
}

std::string ClippingMath::Census() const {
    const auto count = Counts();
    return "box_at=" + std::to_string(count.box_at) + ",box=" + std::to_string(count.box) +
        ",corners=" + std::to_string(count.corners) +
        ",generated_body_box_at=" + std::to_string(count.generated_body_calls[0]) +
        ",generated_body_box_or_corners=" + std::to_string(count.generated_body_calls[1]) +
        ",unsupported_entries=" + std::to_string(count.unsupported_entries) +
        ",unsupported_inputs=" + std::to_string(count.unsupported_inputs);
}

}  // namespace title
