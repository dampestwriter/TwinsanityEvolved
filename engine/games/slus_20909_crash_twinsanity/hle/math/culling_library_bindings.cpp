#include "games/slus_20909_crash_twinsanity/hle/math/culling_library.h"

#include <stdexcept>
#include <utility>

#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/cpu/guest_park.h"

namespace title {

class CullingLibrary::CountsState {
public:
    std::array<std::atomic<uint64_t>, 11> owned{};
    std::array<std::atomic<uint64_t>, 11> generated{};
    std::atomic<uint64_t> interiors{0}, state{0};
};

class CullingLibrary::Installation {
public:
    std::atomic<CullingLibrary*> owner{nullptr};
};

CullingLibrary::~CullingLibrary() {
    // Dispatch must be quiescent during owner teardown. A retained callback
    // subsequently has only its captured original and shared observation state.
    if (installation_) installation_->owner.store(nullptr, std::memory_order_release);
}

bool CullingLibrary::Install(
        const CaptureBody& capture, const Installer& installer, bool selected) {
    if (installation_ || !capture || !installer) return false;
    const auto& entries = Entries();
    std::array<Body, 11> originals;
    for (size_t i = 0; i < entries.size(); ++i) {
        originals[i] = capture(entries[i].address);
        if (!originals[i]) return false;
    }
    if (!counts_) counts_ = std::make_shared<CountsState>();
    auto counts = counts_;
    auto installation = std::make_shared<Installation>();
    const auto rollback = [&](size_t last) {
        for (size_t restored = last + 1; restored != 0; --restored) {
            try { (void)installer(entries[restored - 1].address, originals[restored - 1]); }
            catch (...) {}  // Inactive callbacks still own their counted originals.
        }
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto entry = entries[i];
        // Keep exact original continuation metadata in the control arm. The
        // shared alias lives with each retained callable, including rollback.
        const auto original = DispatchTable::CountCalls(entry.address, originals[i],
            std::shared_ptr<std::atomic<uint64_t>>(counts, &counts->generated[i]));
        Body body = original;
        if (selected)
            body = [installation, counts, original, entry, i](CpuContext& context, uint32_t address) {
                auto* owner = installation->owner.load(std::memory_order_acquire);
                if (!owner) {
                    original(context, address);
                    return;
                }
                // Producers and readers switch together: one original call
                // against owned state (or the opposite) would be incoherent, so
                // a refused call parks the leg with its reason, never aborts.
                if (address != 0 && address != entry.address) {
                    counts->interiors.fetch_add(1, std::memory_order_relaxed);
                    GuestPark::Forever("culling_library_interior", address, entry.name);
                }
                if (!owner->Invoke(entry.operation, context)) {
                    counts->state.fetch_add(1, std::memory_order_relaxed);
                    GuestPark::Forever("culling_library_state", entry.address, entry.name);
                }
                counts->owned[i].fetch_add(1, std::memory_order_relaxed);
            };
        bool installed;
        try { installed = installer(entry.address, std::move(body)); }
        catch (...) {
            rollback(i);
            throw;
        }
        if (!installed) {
            // Include the rejected position: installers may publish then fail.
            // The token has never been activated, so rejected physical rollback
            // still leaves safe counted-original callbacks.
            rollback(i);
            return false;
        }
    }
    installation_ = installation;
    installation->owner.store(this, std::memory_order_release);
    return true;
}

CullingLibrary::Counters CullingLibrary::Counts() const {
    Counters result{};
    result.legacy_imports = legacy_imports_.load(std::memory_order_relaxed);
    if (!counts_) return result;
    for (size_t i = 0; i < result.owned_calls.size(); ++i) {
        result.owned_calls[i] = counts_->owned[i].load(std::memory_order_relaxed);
        result.generated_body_calls[i] = counts_->generated[i].load(std::memory_order_relaxed);
    }
    result.unsupported_interiors = counts_->interiors.load(std::memory_order_relaxed);
    result.unsupported_state = counts_->state.load(std::memory_order_relaxed);
    return result;
}

std::string CullingLibrary::Census() const {
    const auto counts = Counts();
    std::string result;
    for (size_t i = 0; i < Entries().size(); ++i) {
        if (!result.empty()) result += ',';
        result += std::string(Entries()[i].name) + "=" + std::to_string(counts.owned_calls[i]);
        result += ",generated_body_" + std::string(Entries()[i].name) + "=" +
            std::to_string(counts.generated_body_calls[i]);
    }
    result += ",unsupported_interiors=" + std::to_string(counts.unsupported_interiors);
    result += ",unsupported_state=" + std::to_string(counts.unsupported_state);
    result += ",legacy_imports=" + std::to_string(counts.legacy_imports);
    return result;
}

}  // namespace title
