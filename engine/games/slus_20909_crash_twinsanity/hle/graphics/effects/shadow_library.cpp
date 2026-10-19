#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_library.h"

#include <stdexcept>

namespace title {
namespace {
bool Refuse(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
void Add(ShadowPass::Count& a, const ShadowPass::Count& b) {
    a.calls += b.calls; a.replaced += b.replaced; a.model_calls += b.model_calls; a.interior += b.interior;
}
ShadowLibrary::Counters Add(ShadowLibrary::Counters a, const ShadowLibrary::Counters& b) {
    a.loads.reads += b.loads.reads; a.loads.destroys += b.loads.destroys;
    a.loads.converted += b.loads.converted; a.loads.unselected += b.loads.unselected;
    a.loads.failed += b.loads.failed; a.loads.interior += b.loads.interior;
    a.loads.refused += b.loads.refused;
    Add(a.passes.begin, b.passes.begin); Add(a.passes.mesh, b.passes.mesh); Add(a.passes.end, b.passes.end);
    a.passes.instances += b.passes.instances; a.passes.blocks += b.passes.blocks;
    a.passes.groups += b.passes.groups; a.passes.completed += b.passes.completed;
    a.passes.faults += b.passes.faults; a.passes.refused += b.passes.refused;
    a.passes.limit_refusals += b.passes.limit_refusals;
    a.passes.refused_meshes += b.passes.refused_meshes; a.passes.empty += b.passes.empty;
    // The empty census: per-frame counts add; the frame ranges concatenate in
    // order (a retired owner's frames precede the live owner's), bounded as the
    // live owner bounds them; the later owner's last published frame wins.
    a.passes.frames_closed += b.passes.frames_closed;
    a.passes.frames_one_bracket += b.passes.frames_one_bracket;
    a.passes.frames_many_brackets += b.passes.frames_many_brackets;
    a.passes.empty_no_mesh += b.passes.empty_no_mesh;
    a.passes.empty_redirected += b.passes.empty_redirected;
    a.passes.empty_frames_total += b.passes.empty_frames_total;
    a.passes.empty_frames_unlisted += b.passes.empty_frames_unlisted;
    for (const auto& range : b.passes.empty_frames) {
        if (a.passes.empty_frames.size() < ShadowPass::kEmptyFrameRanges) a.passes.empty_frames.push_back(range);
        else ++a.passes.empty_frames_unlisted;
    }
    if (b.passes.last_published_frame) a.passes.last_published_frame = b.passes.last_published_frame;
    // The live owner's last refusal wins; a retired owner's stays when none since.
    if (b.passes.last_mesh_refusal.detail.check != ShadowSource::MeshCheck::None)
        a.passes.last_mesh_refusal = b.passes.last_mesh_refusal;
    a.assets.loaded += b.assets.loaded; a.assets.released += b.assets.released;
    a.assets.rejected += b.assets.rejected; a.restores += b.restores;
    return a;
}
}
struct ShadowLibrary::Impl {
    struct State {
        ShadowAssets assets;
        ShadowLoads loads;
        ShadowPass pass;
        std::map<uint32_t, Body> entries;
        explicit State(const ShadowPass::Hooks& hooks)
            : loads(assets, hooks.read, hooks.refuse), pass(assets, hooks) {}
        Counters Counts() const { return {loads.Counts(), pass.Counts(), assets.Counts(), 0}; }
    };
    ShadowPass::Hooks hooks;
    std::map<uint32_t, Body> originals;
    std::unique_ptr<State> state, candidate;
    ShadowAssets::Snapshot candidate_assets;
    Counters retired;
    bool installed = false;
    explicit Impl(ShadowPass::Hooks value) : hooks(std::move(value)) {}
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
        if ((hooks.enabled && !next->loads.Install(read, bind)) || !next->pass.Install(read, bind)) return {};
        return next;
    }
};
ShadowLibrary::ShadowLibrary(ShadowPass::Hooks hooks) : impl_(new Impl(std::move(hooks))) {}
ShadowLibrary::~ShadowLibrary() = default;
bool ShadowLibrary::Selected() const { return impl_->hooks.enabled; }
ShadowLibrary::Counters ShadowLibrary::Counts() const {
    return impl_->state ? Add(impl_->retired, impl_->state->Counts()) : impl_->retired;
}
bool ShadowLibrary::Install(const Capture& capture, const Installer& install) {
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
        if (!restored) throw std::runtime_error("ShadowLibrary dispatch rollback failed");
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
bool ShadowLibrary::CanCheckpoint(std::string* error, bool unlinked_recording_open) const {
    const auto& owner = *impl_;
    if (!owner.installed || !owner.state || !owner.state->pass.CanCheckpoint(unlinked_recording_open) ||
        (owner.hooks.enabled && !owner.state->loads.CanCheckpoint()))
        return Refuse(error, "Shadow producer is uninstalled, open, pending or faulted");
    if (!owner.hooks.enabled) return true;
    ShadowAssets::Snapshot assets;
    if (owner.state->assets.Capture(assets) != ShadowAssets::Result::Ok)
        return Refuse(error, "Shadow assets retain a consumer");
    return true;
}
bool ShadowLibrary::CaptureState(Snapshot& out, std::string* error, bool unlinked_recording_open) const {
    if (!CanCheckpoint(error, unlinked_recording_open)) return false;
    const auto& owner = *impl_;
    Snapshot snapshot;
    snapshot.selected = owner.hooks.enabled;
    if (snapshot.selected &&
        (owner.state->assets.Capture(snapshot.assets) != ShadowAssets::Result::Ok ||
         !ShadowAssets::ValidateSource(snapshot.assets, owner.hooks.read) ||
         !ShadowAssets::ValidateConsumers(snapshot.assets, owner.hooks.read)))
        return Refuse(error, "Shadow captured asset identity differs from live source RAM");
    out = std::move(snapshot);
    return true;
}
void ShadowLibrary::DiscardStaged() {
    impl_->candidate.reset();
    impl_->candidate_assets = {};
}
bool ShadowLibrary::Stage(const Snapshot& snapshot, const MaterialState::Read& incoming, std::string* error) {
    DiscardStaged();
    auto& owner = *impl_;
    if (!owner.installed || snapshot.selected != owner.hooks.enabled)
        return Refuse(error, "Shadow checkpoint selection differs from the live owner");
    if ((!snapshot.selected && (!snapshot.assets.assets.empty() || snapshot.assets.next_generation != 1 ||
                                snapshot.assets.version != 1)) ||
        (snapshot.selected && (!ShadowAssets::ValidateSource(snapshot.assets, incoming) ||
                               !ShadowAssets::ValidateConsumers(snapshot.assets, incoming))))
        return Refuse(error, "Shadow incoming asset bytes or source identities are invalid");
    auto next = owner.Build({});
    if (!next) return Refuse(error, "Shadow restored owner has no captured source bodies");
    if (snapshot.selected && next->assets.Restore(snapshot.assets) != ShadowAssets::Result::Ok)
        return Refuse(error, "Shadow restored asset generation cannot be constructed");
    owner.candidate_assets = snapshot.assets;
    owner.candidate = std::move(next);
    return true;
}
bool ShadowLibrary::Publish(std::string* error) {
    auto& owner = *impl_;
    if (!owner.candidate || !CanCheckpoint(error))
        return Refuse(error, "Shadow publication requires a staged owner and completed output");
    if (owner.hooks.enabled && (!ShadowAssets::ValidateSource(owner.candidate_assets, owner.hooks.read) ||
                               !ShadowAssets::ValidateConsumers(owner.candidate_assets, owner.hooks.read)))
        return Refuse(error, "Shadow staged identities differ from the restored RAM");
    owner.retired = Add(owner.retired, owner.state->Counts());
    ++owner.retired.restores;
    owner.state.swap(owner.candidate);
    DiscardStaged();
    return true;
}
}  // namespace title
