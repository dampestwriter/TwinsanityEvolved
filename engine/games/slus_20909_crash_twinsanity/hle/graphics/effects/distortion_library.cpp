#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_library.h"

#include <atomic>
#include <map>
#include <stdexcept>
#include <vector>

namespace title {
namespace {
constexpr std::array<uint32_t, 3> kBlockEntries{
    DistortionBlocks::kInitialize, DistortionBlocks::kResize, DistortionBlocks::kEvents};
bool Refuse(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
DistortionLibrary::Counters Add(DistortionLibrary::Counters a, const DistortionLibrary::Counters& b) {
    a.blocks.initializes += b.blocks.initializes; a.blocks.resizes += b.blocks.resizes;
    a.blocks.event_turns += b.blocks.event_turns; a.blocks.captured_blocks += b.blocks.captured_blocks;
    a.blocks.failures += b.blocks.failures;
    a.settings.calls += b.settings.calls; a.settings.captured += b.settings.captured;
    a.settings.rejected += b.settings.rejected; a.settings.interior += b.settings.interior;
    a.pass.calls += b.pass.calls; a.pass.replaced += b.pass.replaced; a.pass.model_calls += b.pass.model_calls;
    a.pass.interior += b.pass.interior; a.pass.forgets += b.pass.forgets; a.pass.batches += b.pass.batches;
    a.pass.blocks += b.pass.blocks; a.pass.draws += b.pass.draws; a.pass.completed += b.pass.completed;
    a.pass.faults += b.pass.faults; a.pass.refused += b.pass.refused; a.restores += b.restores;
    return a;
}
bool EmptyModelState(const DistortionLibrary::Snapshot& snapshot) {
    return !snapshot.blocks.generation && !snapshot.blocks.pool && snapshot.blocks.next.empty() &&
        snapshot.blocks.version == 1 && !snapshot.settings.shader && !snapshot.settings.generation &&
        snapshot.settings.version == 1;
}
}
struct DistortionLibrary::Impl {
    struct State {
        DistortionBlocks blocks;
        DistortionSettings settings;
        DistortionPass pass;
        std::map<uint32_t, Body> entries;
        explicit State(DistortionPass::Hooks hooks)
            : blocks(hooks.read), settings(hooks.read), pass(blocks, Versioned(std::move(hooks))) {}
        DistortionPass::Hooks Versioned(DistortionPass::Hooks hooks) {
            hooks.settings = [this](uint32_t shader, DistortionMaterial::Settings& out) {
                return settings.Current(shader, out);
            };
            return hooks;
        }
        Counters Counts() const { return {blocks.Counts(), settings.Counts(), pass.Counts(), 0}; }
    };
    DistortionPass::Hooks hooks;
    std::map<uint32_t, Body> originals;
    std::unique_ptr<State> state, candidate;
    Counters retired;
    bool installed = false;
    explicit Impl(DistortionPass::Hooks value) : hooks(std::move(value)) {}
    std::unique_ptr<State> Build(const Capture& capture) {
        auto next = std::make_unique<State>(hooks);
        const auto read = [&](uint32_t address) -> Body {
            const auto local = next->entries.find(address);
            if (local != next->entries.end()) return local->second;
            auto found = originals.find(address);
            if (found == originals.end()) {
                const auto body = capture ? capture(address) : Body{};
                if (!body) return {};
                found = originals.emplace(address, body).first;
            }
            return found->second;
        };
        const auto bind = [&](uint32_t address, Body body) {
            if (!body) return false;
            next->entries[address] = std::move(body);
            return true;
        };
        if (hooks.enabled) {
            for (const uint32_t address : kBlockEntries) {
                const auto original = read(address);
                if (!original) return {};
                auto* blocks = &next->blocks;
                bind(address, [blocks, original, address](CpuContext& c, uint32_t at) {
                    blocks->Observe(address, c, at, original);
                });
            }
            if (!next->settings.Install(read, bind)) return {};
        }
        if (!next->pass.Install(read, bind)) return {};
        return next;
    }
};
DistortionLibrary::DistortionLibrary(DistortionPass::Hooks hooks) : impl_(new Impl(std::move(hooks))) {}
DistortionLibrary::~DistortionLibrary() = default;
bool DistortionLibrary::Selected() const { return impl_->hooks.enabled; }
DistortionLibrary::Counters DistortionLibrary::Counts() const {
    return impl_->state ? Add(impl_->retired, impl_->state->Counts()) : impl_->retired;
}
bool DistortionLibrary::Install(const Capture& capture, const Installer& install) {
    auto& owner = *impl_;
    if (owner.installed || !capture || !install) return false;
    auto state = owner.Build(capture);
    if (!state) return false;
    const auto active = std::make_shared<std::atomic<bool>>(false);
    std::vector<uint32_t> published;
    published.reserve(state->entries.size());
    const auto rollback = [&] {
        bool restored = true;
        for (auto at = published.rbegin(); at != published.rend(); ++at) {
            try { if (!install(*at, owner.originals.at(*at))) restored = false; }
            catch (...) { restored = false; }
        }
        if (!restored) throw std::runtime_error("DistortionLibrary dispatch rollback failed");
    };
    for (const auto& entry : state->entries) {
        const uint32_t address = entry.first;
        const auto original = owner.originals.at(address);
        published.push_back(address); // a rejecting installer may have published it
        bool accepted = false;
        try {
            accepted = install(address, [this, active, original, address](CpuContext& c, uint32_t at) {
                if (!active->load(std::memory_order_acquire)) { original(c, at); return; }
                impl_->state->entries.at(address)(c, at);
            });
        } catch (...) { rollback(); throw; }
        if (!accepted) { rollback(); return false; }
    }
    owner.state = std::move(state);
    owner.installed = true;
    active->store(true, std::memory_order_release);
    return true;
}
bool DistortionLibrary::CanCheckpoint(std::string* error, bool unlinked_recording_open) const {
    const auto& owner = *impl_;
    if (!owner.installed || !owner.state || !owner.state->pass.CanCheckpoint(unlinked_recording_open) ||
        (owner.hooks.enabled && (!owner.state->settings.CanCheckpoint() || owner.state->blocks.Busy())))
        return Refuse(error, "Distortion producer is uninstalled, open, pending or faulted");
    return true;
}
bool DistortionLibrary::CaptureState(Snapshot& out, std::string* error, bool unlinked_recording_open) const {
    if (!CanCheckpoint(error, unlinked_recording_open)) return false;
    const auto& owner = *impl_;
    Snapshot snapshot;
    snapshot.selected = owner.hooks.enabled;
    if (snapshot.selected && (!owner.state->blocks.CaptureState(snapshot.blocks) ||
                              !owner.state->settings.CaptureState(snapshot.settings)))
        return Refuse(error, "Distortion owners are busy");
    out = std::move(snapshot);
    return true;
}
void DistortionLibrary::DiscardStaged() { impl_->candidate.reset(); }
bool DistortionLibrary::Stage(const Snapshot& snapshot, const MaterialState::Read& incoming, std::string* error) {
    DiscardStaged();
    auto& owner = *impl_;
    if (!owner.installed || snapshot.selected != owner.hooks.enabled)
        return Refuse(error, "Distortion checkpoint selection differs from the live owner");
    if (!snapshot.selected) {
        if (!EmptyModelState(snapshot)) return Refuse(error, "Model-only distortion state carries owned values");
    } else {
        if (!DistortionBlocks::Validate(snapshot.blocks) || !DistortionSettings::Validate(snapshot.settings))
            return Refuse(error, "Distortion incoming block/settings values are invalid");
        // The versioned shader must still be the retail type18 object in the
        // incoming RAM; its later linear/K/L writes are expected and ignored.
        DistortionMaterial::Settings probe;
        if (snapshot.settings.shader && (!incoming ||
            DistortionMaterial::CaptureSettings(incoming, snapshot.settings.shader, probe) !=
                DistortionMaterial::Result::Ok))
            return Refuse(error, "Distortion settings version names no retail material in incoming RAM");
    }
    auto next = owner.Build({});
    if (!next) return Refuse(error, "Distortion restored owner has no captured source bodies");
    if (snapshot.selected && (!next->blocks.Restore(snapshot.blocks) || !next->settings.Restore(snapshot.settings)))
        return Refuse(error, "Distortion restored values cannot be constructed");
    owner.candidate = std::move(next);
    return true;
}
bool DistortionLibrary::Publish(std::string* error) {
    auto& owner = *impl_;
    if (!owner.candidate || !CanCheckpoint(error))
        return Refuse(error, "Distortion publication requires a staged owner and completed output");
    owner.retired = Add(owner.retired, owner.state->Counts());
    ++owner.retired.restores;
    owner.state.swap(owner.candidate);
    DiscardStaged();
    return true;
}
void DistortionLibrary::ResetAfterQuiescence() {
    if (impl_->state) impl_->state->pass.ResetAfterQuiescence();
}
}  // namespace title
