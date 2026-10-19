#include "games/slus_20909_crash_twinsanity/hle/math/bounds_math.h"

#include <stdexcept>
#include <utility>

#include "engine/ee/cpu/dispatch_table.h"

namespace title {

class BoundsMath::CountsState {
public:
    std::array<std::atomic<uint64_t>, 3> owned{}, generated{};
    std::atomic<uint64_t> interiors{0}, unsupported{0};
};
class BoundsMath::Installation {
public:
    std::atomic<BoundsMath*> owner{nullptr};
};

BoundsMath::~BoundsMath() {
    if (installation_) installation_->owner.store(nullptr, std::memory_order_release);
}

bool BoundsMath::Install(const CaptureBody& capture, const Installer& installer, bool selected) {
    if (installation_ || !capture || !installer) return false;
    const auto& entries = Entries();
    std::array<Body, 3> originals;
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
            catch (...) {}  // Rejected rollback callbacks remain inactive.
        }
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto entry = entries[i];
        const auto original = DispatchTable::CountCalls(entry.address, originals[i],
            std::shared_ptr<std::atomic<uint64_t>>(counts, &counts->generated[i]));
        Body body = original;
        if (selected) {
            auto interior = DispatchTable::CountCalls(entry.address, original,
                std::shared_ptr<std::atomic<uint64_t>>(counts, &counts->interiors));
            body = DispatchTable::DelegateInterior(entry.address, std::move(interior),
                [installation, counts, original, entry, i](CpuContext& context, uint32_t address) {
                    auto* owner = installation->owner.load(std::memory_order_acquire);
                    if (!owner) { original(context, address); return; }
                    if (!owner->Invoke(entry.address, context)) {
                        counts->unsupported.fetch_add(1, std::memory_order_relaxed);
                        throw std::runtime_error("BoundsMath: invalid source point span");
                    }
                    counts->owned[i].fetch_add(1, std::memory_order_relaxed);
                });
        }
        bool installed;
        try { installed = installer(entry.address, std::move(body)); }
        catch (...) { rollback(i); throw; }
        if (!installed) { rollback(i); return false; }
    }
    installation_ = installation;
    installation->owner.store(this, std::memory_order_release);
    return true;
}

BoundsMath::Counters BoundsMath::Counts() const {
    Counters result{};
    if (!counts_) return result;
    for (size_t i = 0; i < Entries().size(); ++i) {
        result.owned_calls[i] = counts_->owned[i].load(std::memory_order_relaxed);
        result.generated_body_calls[i] = counts_->generated[i].load(std::memory_order_relaxed);
    }
    result.generated_interiors = counts_->interiors.load(std::memory_order_relaxed);
    result.unsupported_inputs = counts_->unsupported.load(std::memory_order_relaxed);
    return result;
}

std::string BoundsMath::Census() const {
    const auto counts = Counts();
    std::string result;
    for (size_t i = 0; i < Entries().size(); ++i) {
        if (!result.empty()) result += ',';
        result += std::string(Entries()[i].name) + "=" + std::to_string(counts.owned_calls[i]);
        result += ",generated_body_" + std::string(Entries()[i].name) + "=" +
            std::to_string(counts.generated_body_calls[i]);
    }
    return result + ",generated_interiors=" + std::to_string(counts.generated_interiors) +
        ",unsupported_inputs=" + std::to_string(counts.unsupported_inputs);
}

}  // namespace title
