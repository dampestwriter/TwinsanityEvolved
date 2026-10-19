#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_pass.h"

#include <stdexcept>

namespace title {
namespace {
constexpr std::array<uint32_t, 5> entries{
    ShadowSource::kBegin, ShadowSource::kMesh, ShadowSource::kEnd, ShadowPass::kClose, ShadowPass::kForget};
ShadowDraw::Frame Logical(const ShadowPass::Frame& frame) {
    return {frame.source_frame, frame.width, frame.height};
}
}
bool ShadowPass::Batch::Ready() const { return state_.load(std::memory_order_acquire) == State::Ready; }
bool ShadowPass::Batch::Complete() const { return state_.load(std::memory_order_acquire) == State::Complete; }
bool ShadowPass::Batch::Failed() const { return state_.load(std::memory_order_acquire) == State::Failed; }
const std::vector<ps2r::gs::DrawIr>& ShadowPass::Batch::Draws() const {
    if (!Ready()) throw std::logic_error("Shadow batch is not ready");
    return draws_;
}
bool ShadowPass::Batch::Finish(bool success) const {
    State expected = State::Ready;
    if (state_.compare_exchange_strong(expected, success ? State::Complete : State::Failed,
                                       std::memory_order_acq_rel)) return success;
    // Repeated delivery or a pre-closure callback cannot resurrect the batch.
    state_.store(State::Failed, std::memory_order_release);
    return false;
}
ShadowPass::ShadowPass(ShadowAssets& assets, Hooks hooks) : assets_(assets), hooks_(std::move(hooks)) {}
bool ShadowPass::Install(const Capture& capture, const Installer& install) {
    if (installed_) return true;
    if (!capture || !install || (hooks_.enabled &&
        (!hooks_.read || !hooks_.write || !hooks_.frame || !hooks_.reserve))) return false;
    for (uint32_t address : entries) {
        auto body = capture(address);
        if (!body) { originals_.clear(); return false; }
        originals_[address] = std::move(body);
    }
    const auto active = std::make_shared<std::atomic<bool>>(false);
    const auto rollback = [&](size_t last) {
        bool restored = true;
        for (size_t count = last + 1; count != 0; --count) {
            const auto address = entries[count - 1];
            try { if (!install(address, originals_.at(address))) restored = false; }
            catch (...) { restored = false; }
        }
        originals_.clear();
        if (!restored) throw std::runtime_error("ShadowPass dispatch rollback failed");
    };
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto address = entries[i];
        const auto original = originals_.at(address);
        bool accepted = false;
        try {
            accepted = install(address, [this, active, original, address](CpuContext& c, uint32_t e) {
                if (!active->load(std::memory_order_acquire)) { original(c, e); return; }
                Run(address, c, e);
            });
        } catch (...) { rollback(i); throw; }
        if (!accepted) { rollback(i); return false; }
    }
    installed_ = true;
    active->store(true, std::memory_order_release);
    return true;
}
[[noreturn]] void ShadowPass::Fault(const char* text) {
    if (!faulted_) ++counts_.faults;
    faulted_ = true;
    throw std::runtime_error(text);
}
void ShadowPass::Collect() {
    for (auto at = pending_.begin(); at != pending_.end();) {
        if ((*at)->Failed()) Fault("Shadow ordered consumer failed");
        if ((*at)->Complete()) {
            ++counts_.completed;
            at = pending_.erase(at);
        } else ++at;
    }
}
bool ShadowPass::Begin() {
    Collect();
    if (open_ || closed_) Fault("Shadow begin crossed an unfinished source boundary");
    Frame frame;
    ShadowSource::BeginPlan plan;
    // Nothing is reserved or written yet: an unavailable source frame or an
    // unchecked matrix refuses this bracket to the model instead of faulting.
    if (!hooks_.frame(frame) || !frame.epoch ||
        ShadowSource::Begin(hooks_.read, Logical(frame), plan) != ShadowSource::Result::Ok)
        return false;
    // The pass keeps at most kPendingLimit reservations the ordered output has
    // not delivered. When the output stalls (allon_w2: the owned sky's missing
    // VU1 program residency sent ~1 M kicks a route down the raw PATH1 route and
    // the frames behind it never completed; play faulted "Shadow pass limit" at
    // frame ~5000 with 3 GB resident), a further bracket is REFUSED to the model
    // -- counted (refused, limit_refusals), attributed (producer kBegin, detail
    // kLimitDetail, ordering) -- never a fault of the run: the output owes the
    // pass its earlier deliveries, nothing about this frame is wrong.
    if (batches_.size() >= kPendingLimit || pending_.size() >= kPendingLimit) {
        ++counts_.limit_refusals;
        if (hooks_.refuse) hooks_.refuse(GraphicsRefusals::Reason::Ordering, ShadowSource::kBegin, kLimitDetail);
        return false;
    }
    auto batch = std::make_shared<Batch>(frame);
    // Allocate producer tracking before any root queue or guest mutation.
    batches_.reserve(batches_.size() + 1);
    pending_.reserve(pending_.size() + 1);
    // RecordOwned false = "stay unselected for this call" (INTERFACES section 8):
    // the output queue is unchanged, so the bracket refuses to the model.
    if (!hooks_.reserve(batch)) return false;
    batches_.push_back(batch);
    pending_.push_back(batch);
    if (!hooks_.write(plan.writes)) Fault("Shadow begin matrix publication failed");
    source_view_ = plan.view;
    open_ = std::move(batch);
    return true;
}
bool ShadowPass::Mesh(const CpuContext& context) {
    if (!open_ || closed_) Fault("Shadow mesh outside the source pass");
    ShadowSource::Instance instance;
    ShadowSource::MeshDetail detail;
    const auto result = ShadowSource::Mesh(hooks_.read, assets_, Logical(open_->frame), context, instance, &detail);
    if (result != ShadowSource::Result::Ok) {
        // Nothing is recorded for this call. The bracket is already committed
        // (its begin bypassed StartShadows' half-size target set-up), so the
        // model body cannot draw this mesh coherently: the call is refused by
        // name and the mesh is drawn by neither arm. Never a fault, never garbage.
        ++counts_.refused_meshes;
        counts_.last_mesh_refusal = {result, detail};
        if (hooks_.refuse)
            hooks_.refuse(detail.check == ShadowSource::MeshCheck::Asset ?
                              GraphicsRefusals::Reason::MissingResource :
                              GraphicsRefusals::Reason::UnsupportedState,
                          ShadowSource::kMesh, ShadowSource::Detail(result, detail.check));
        return false;
    }
    auto material = materials_.find(instance.material);
    if (material == materials_.end()) {
        material_order_.push_back(instance.material);
        material = materials_.emplace(instance.material, Material{open_, {}}).first;
    }
    const auto& owner = material->second.batch->frame;
    // Original material insertion inherits the FIRST pass's XY offset.
    // Later source instances contribute their own already captured matrices.
    instance.view.offset = {float(2048 - (owner.width >> 2)), float(2048 - (owner.height >> 2))};
    const auto found = active_.find(instance.object);
    std::shared_ptr<Block> block;
    if (found == active_.end()) {
        if (counts_.blocks == UINT64_MAX) Fault("Shadow block census exhausted");
        size_t blocks = 0;
        for (const auto& pair : materials_) blocks += pair.second.blocks.size();
        if (blocks >= 800) Fault("Shadow instance block limit");
        block = std::make_shared<Block>();
        block->object = instance.object;
        block->material = instance.material;
        block->asset = instance.asset;
        block->eye = instance.eye;
        active_.emplace(instance.object, block);
        material->second.blocks.push_back(block);
        ++counts_.blocks;
    } else {
        block = found->second;
        if (block->asset != instance.asset || block->material != instance.material ||
            block->eye != instance.eye)
            Fault("Shadow source object/material changed within an instance block");
    }
    block->views.push_back(instance.view);
    block->quadwords += ShadowSource::InstanceSize(instance);
    ++counts_.instances;
    ++open_->meshes_;
    // Source SetPlacedModelRenderDMA: the first instance opens the block; the
    // block closes once size + PlacedRoom reaches its capacity -- the eighth
    // eye-sized instance (88 + 14 >= 100), the ninth clipped one (90 + 14).
    if (block->quadwords + ShadowSource::kRoom >= ShadowSource::kCapacity) active_.erase(instance.object);
    return true;
}
void ShadowPass::End() {
    if (!open_) Fault("Shadow end has no matching begin");
    ShadowSource::Write write;
    if (ShadowSource::End(hooks_.read, source_view_, write) != ShadowSource::Result::Ok ||
        !hooks_.write({write})) Fault("Shadow view matrix restore failed");
    open_.reset();
    source_view_ = 0;
}
void ShadowPass::Close() {
    if (open_) Fault("Shadow source instance closure inside a pass");
    if (closed_) return;
    // All output is staged before any batch becomes visible to its consumer.
    std::map<Batch*, std::vector<ps2r::gs::DrawIr>> staged;
    for (uint32_t address : material_order_) {
        const auto& material = materials_.at(address);
        auto& draws = staged[material.batch.get()];
        for (const auto& block : material.blocks) {
            std::vector<ps2r::gs::DrawIr> group;
            if (ShadowGeometry::Build(block->asset->mesh, block->views, group) != ShadowGeometry::Result::Ok)
                Fault("Shadow owned geometry build failed");
            size_t count = 0;
            for (const auto& draw : draws) count += draw.verts.size();
            for (const auto& draw : group) count += draw.verts.size();
            if (count > ShadowDraw::kMaxVertices) Fault("Shadow output vertex limit");
            draws.insert(draws.end(), std::make_move_iterator(group.begin()), std::make_move_iterator(group.end()));
        }
    }
    for (const auto& batch : batches_) {
        if (batch->state_.load(std::memory_order_acquire) != Batch::State::Building)
            Fault("Shadow source work consumed before closure");
        auto& draws = staged[batch.get()];
        std::vector<ShadowDraw::Command> checked;
        if (ShadowDraw::Build(Logical(batch->frame), draws, checked) != ShadowDraw::Result::Ok)
            Fault("Shadow complete pass validation failed");
    }
    bool published = false;
    uint32_t frame = 0;
    for (const auto& batch : batches_) {
        batch->draws_ = std::move(staged[batch.get()]);
        counts_.groups += batch->draws_.size();
        frame = batch->frame.source_frame;
        if (batch->draws_.empty()) {
            ++counts_.empty;
            if (batch->meshes_) ++counts_.empty_redirected; else ++counts_.empty_no_mesh;
        } else published = true;
        auto expected = Batch::State::Building;
        if (!batch->state_.compare_exchange_strong(expected, Batch::State::Ready, std::memory_order_release))
            Fault("Shadow source batch failed during closure");
    }
    if (!batches_.empty()) {
        ++counts_.frames_closed;
        if (batches_.size() == 1) ++counts_.frames_one_bracket; else ++counts_.frames_many_brackets;
        if (published) counts_.last_published_frame = frame;
        else {
            ++counts_.empty_frames_total;
            auto& ranges = counts_.empty_frames;
            if (!ranges.empty() && ranges.back().last + 1 == frame) ranges.back().last = frame;
            else if (ranges.size() < kEmptyFrameRanges) ranges.push_back({frame, frame});
            else ++counts_.empty_frames_unlisted;
        }
    }
    active_.clear();
    closed_ = true;
}
void ShadowPass::Forget() {
    if (open_ || (!batches_.empty() && !closed_)) Fault("Shadow source work forgotten before closure");
    batches_.clear();
    material_order_.clear();
    materials_.clear();
    active_.clear();
    closed_ = false;
    Collect();
}
void ShadowPass::Run(uint32_t boundary, CpuContext& context, uint32_t entry) try {
    if (!installed_ || !originals_.count(boundary) || faulted_)
        Fault("Shadow producer has no valid continuation");
    Count* count = boundary == ShadowSource::kBegin ? &counts_.begin :
                   boundary == ShadowSource::kMesh ? &counts_.mesh :
                   boundary == ShadowSource::kEnd ? &counts_.end : nullptr;
    if (count) ++count->calls;
    const bool whole = !entry || entry == boundary;
    if (!whole && count) ++count->interior;
    if (!hooks_.enabled) {
        if (count) ++count->model_calls;
        originals_.at(boundary)(context, entry);
        return;
    }
    if (!whole) Fault("Selected shadow producer cannot import an interior continuation");
    if (boundary == ShadowSource::kBegin && !Begin()) {
        // The whole bracket runs the model: its meshes and end follow below.
        ++counts_.refused;
        refusing_ = true;
        ++count->model_calls;
        originals_.at(boundary)(context, entry);
        return;
    }
    if (refusing_ && (boundary == ShadowSource::kMesh || boundary == ShadowSource::kEnd)) {
        ++count->model_calls;
        originals_.at(boundary)(context, entry);
        if (boundary == ShadowSource::kEnd) refusing_ = false;
        return;
    }
    if (boundary == ShadowSource::kBegin) {}
    else if (boundary == ShadowSource::kMesh) {
        // A refused mesh is still the owner's call (replaced): the model body
        // would draw its strips straight into the scene without the bypassed
        // source set-up. Its refusal is counted and attributed in Mesh.
        Mesh(context);
    }
    else if (boundary == ShadowSource::kEnd) End();
    else if (boundary == kClose) { Close(); originals_.at(boundary)(context, entry); }
    else if (boundary == kForget) { Forget(); originals_.at(boundary)(context, entry); }
    if (count) ++count->replaced;
} catch (...) {
    if (!faulted_) { faulted_ = true; ++counts_.faults; }
    throw;
}
bool ShadowPass::CanCheckpoint(bool unlinked_recording_open) const {
    if (faulted_ || open_ || refusing_ || !active_.empty()) return false;
    if (!unlinked_recording_open && (!batches_.empty() || !materials_.empty())) return false;
    // A settled boundary: this frame's pass is closed (Ready batches behind
    // their predecessor, materials consumed), awaiting the Link that the
    // image does not carry (GXPD); a Building batch is still an open pass.
    if (!batches_.empty() && !closed_) return false;
    for (const auto& batch : pending_)
        if (!batch->Complete() && !(unlinked_recording_open && batch->Ready())) return false;
    return true;
}
ShadowPass::Counters ShadowPass::Counts() const {
    auto result = counts_;
    for (const auto& batch : pending_) if (batch->Complete()) ++result.completed;
    return result;
}
void ShadowPass::ResetAfterQuiescence() {
    open_.reset();
    batches_.clear(); pending_.clear();
    materials_.clear(); material_order_.clear(); active_.clear();
    source_view_ = 0;
    faulted_ = closed_ = refusing_ = false;
}
}  // namespace title
