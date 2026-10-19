#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

#include <algorithm>
#include <stdexcept>

namespace title {
namespace {
bool Bad(std::string* error, const char* why) {
    if (error) *error = std::string("Graphics shared surfaces: ") + why;
    return false;
}
}

GraphicsScene::~GraphicsScene() { DiscardStagedState(); }

bool GraphicsScene::CanCaptureState(std::string* error) const {
    if (!SceneState::ValidateViewport(CaptureViewport(),error)) return false;
    auto& cache = retained_.Cache();
    const auto* sequence = retained_.Sequence();
    if (sequence && sequence->GetState() != ps2r::gs::SequenceRecipe::kIdle)
        return Bad(error,"active shadow dependency");
    if (!cache.Ledger().Empty()) return Bad(error,"pending retained writeback dependency");
    bool ready = cache.Count() <= 64;
    uint64_t bytes = 68;
    std::string offender;
    cache.ForEach([&](const ps2r::gs::RenderTarget& t) {
        const auto& id = t.identity;
        Frame frame{};
        frame.color_buffer = id.fbp;
        // Capturable: the attachments carry the target whole -- any CT frame
        // format (a 16-bit target draws into the same RGBA8 attachment; GSCN
        // v3 keeps its format and history), never an alt16 companion (a second
        // attachment the codec does not carry), no foreign page or writeback
        // owed to it, no running sequence.
        std::string history;
        if (!Capturable(frame,t) || t.views.HasAlt16() ||
            !SceneState::ValidateTarget(id,t.coherence,retained_.DrawOrder(),&history)) {
            if (ready) offender = " (target fbp=" + std::to_string(id.fbp) + " psm=" + std::to_string(id.render_psm) +
                " fbw=" + std::to_string(id.render_fbw) + " " + std::to_string(id.width) + "x" + std::to_string(id.height) +
                " was_16bit=" + std::to_string(id.was_16bit) + " alt16=" + std::to_string(t.views.HasAlt16()) +
                " capturable=" + std::to_string(Capturable(frame,t)) + (history.empty() ? "" : " " + history) + ")";
            ready = false;
        }
        if (!ready) return;
        bytes += 216 + uint64_t(id.PhysicalW()) * id.PhysicalH() *
            (t.coherence.cd_copy.valid ? 12 : 8);
    });
    if (!ready || bytes > SceneState::kMaxBytes) {
        if (error) *error = "Graphics shared surfaces: unsupported target/alias state or attachment budget" + offender +
            " targets=" + std::to_string(cache.Count()) + " bytes=" + std::to_string(bytes);
        return false;
    }
    if (error) error->clear();
    return true;
}

bool GraphicsScene::SettleForCapture(std::string* error) {
    auto& cache = retained_.Cache();
    const auto* sequence = retained_.Sequence();
    if (sequence && sequence->GetState() != ps2r::gs::SequenceRecipe::kIdle)
        return Bad(error,"active shadow dependency");
    if (!cache.Ledger().Empty()) {
        cache.FlushPending(ps2r::gs::PendingRtSyncLedger::Reason::Save, 0, 0);
        ++capture_settles_;
    }
    if (!cache.Ledger().Empty()) return Bad(error,"pending retained writeback dependency survived the settle");
    if (error) error->clear();
    return true;
}

bool GraphicsScene::CaptureState(SceneState::Snapshot& out, std::string* error) {
    if (!CanCaptureState(error)) return false;
    SceneState::Snapshot snapshot;
    snapshot.display_frame = retained_.Present().FrameCount();
    snapshot.game_frame = retained_.Present().EeFrameCount();
    snapshot.order = retained_.DrawOrder();
    snapshot.epoch = retained_.GfxEpoch();
    snapshot.recent = retained_.Cache().MostRecentDrawnFbp();
    snapshot.viewport = CaptureViewport();
    retained_.Cache().ForEach([&](const ps2r::gs::RenderTarget& target) {
        SceneState::Target saved;
        saved.identity = target.identity;
        saved.history = target.coherence;
        const auto& id = saved.identity;
        const ps2r::render::Rect rect{0,0,id.PhysicalW(),id.PhysicalH()};
        const size_t count = size_t(rect.w) * rect.h;
        saved.color.resize(count * 4);
        saved.depth.resize(count);
        surfaces_.Readback(target.views.color,rect,saved.color.data(),size_t(rect.w) * 4);
        surfaces_.Readback(target.views.depth,rect,saved.depth.data(),size_t(rect.w) * 4);
        if (saved.history.cd_copy.valid) {
            saved.copy.resize(count * 4);
            surfaces_.Readback(target.views.rt_copy,rect,saved.copy.data(),size_t(rect.w) * 4);
        }
        snapshot.targets.push_back(std::move(saved));
    });
    std::sort(snapshot.targets.begin(),snapshot.targets.end(),
        [](const auto& a, const auto& b) { return a.identity.fbp < b.identity.fbp; });
    if (!SceneState::Validate(snapshot,error)) return false;
    out = std::move(snapshot);
    return true;
}

SceneState::Viewport GraphicsScene::CaptureViewport() const {
    const auto& view = retained_.Scanout();
    return {uint32_t(retained_.WindowW()),uint32_t(retained_.WindowH()),
        uint32_t(view.ContentW()),uint32_t(view.ContentH()),view.ScanoutFbp(),
        view.DispfbFbw(),view.DispfbPsm()};
}

void GraphicsScene::DiscardStagedState() {
    for (auto& target : staged_) if (target) target->Destroy(surfaces_);
    staged_.clear();
    staged_history_ = {};
    staged_valid_ = false;
    surfaces_.CollectAfterQuiescence();
}

bool GraphicsScene::StageState(const SceneState::Snapshot& snapshot, std::string* error) {
    DiscardStagedState();
    if (!SceneState::Validate(snapshot,error) || !CanCaptureState(error)) return false;
    const int maximum = surfaces_.QueryCaps().max_texture_size;
    for (const auto& t : snapshot.targets)
        if (t.identity.PhysicalW() > maximum || t.identity.PhysicalH() > maximum ||
            t.identity.scale != retained_.Cache().TargetScale(t.identity.fbp))
            return Bad(error,"attachment exceeds backend limit or configured scale differs");
    try {
        for (const auto& saved : snapshot.targets) {
            staged_.push_back(std::make_unique<ps2r::gs::RenderTarget>());
            auto& target = *staged_.back();
            target.identity = saved.identity;
            target.coherence = saved.history;
            const auto& id = saved.identity;
            ps2r::render::SurfaceDesc desc;
            desc.width = uint32_t(id.PhysicalW()); desc.height = uint32_t(id.PhysicalH());
            desc.scale = id.scale; desc.renderable = true;
            const ps2r::render::Rect rect{0,0,int32_t(desc.width),int32_t(desc.height)};
            const auto upload = [&](ps2r::render::SurfaceId& slot, const void* data) {
                slot = surfaces_.CreateSurface(desc);
                if (!slot.id) throw std::runtime_error("allocation failed");
                if (data) surfaces_.Upload(slot,rect,data,size_t(desc.width) * 4);
            };
            upload(target.views.color,saved.color.data());
            desc.format = ps2r::render::SurfaceFormat::Depth32f;
            desc.renderable = false;
            upload(target.views.depth,saved.depth.data());
            desc.format = ps2r::render::SurfaceFormat::Rgba8;
            upload(target.views.rt_copy,saved.copy.empty() ? nullptr : saved.copy.data());
        }
        // Only metadata survives staging; avoid retaining a second pixel copy.
        staged_history_.display_frame = snapshot.display_frame;
        staged_history_.game_frame = snapshot.game_frame;
        staged_history_.recent = snapshot.recent;
        staged_history_.order = snapshot.order;
        staged_history_.epoch = snapshot.epoch;
        staged_history_.viewport = snapshot.viewport;
        staged_valid_ = true;
        return true;
    } catch (...) {
        DiscardStagedState();
        return Bad(error,"attachment staging failed");
    }
}

bool GraphicsScene::PublishState(std::string* error) {
    if (!staged_valid_) return Bad(error,"no complete staged image");
    // Restore may have invalidated legacy textures after incoming RAM/VRAM
    // publication. No target is read or rescued from that abandoned timeline.
    auto* sequence = retained_.Sequence();
    if ((sequence && sequence->GetState() != ps2r::gs::SequenceRecipe::kIdle) ||
        !retained_.Cache().Ledger().Empty()) return Bad(error,"owner lost quiescence");
    retained_.ForgetTextureViewsAfterRestore();
    if (!retained_.Cache().ReplaceAfterQuiescence(staged_,staged_history_.recent))
        return Bad(error,"target publication failed");
    retained_.Present().RestoreTimeline(staged_history_.display_frame,staged_history_.game_frame);
    retained_.RestoreDrawOrder(staged_history_.order);
    retained_.SetGfxEpoch(staged_history_.epoch);
    const auto& view = staged_history_.viewport;
    retained_.OnSetWindow(int(view.width),int(view.height),int(view.content_width),int(view.content_height));
    retained_.Scanout().RestoreView(view.color_key,view.buffer_width,view.format,
                                    int(view.content_width),int(view.content_height));
    versions_.clear(); // queued leases cannot be present at this boundary
    staged_valid_ = false;
    staged_history_ = {};
    CollectAfterQuiescence();
    if (error) error->clear();
    return true;
}

bool GraphicsScene::RetainStagedColor(Graphics& queue,const Frame& frame,Graphics::Lease& out) {
    if (!staged_valid_) return false;
    for (const auto& target : staged_) {
        const auto& id = target->identity;
        if (id.fbp != frame.color_buffer) continue;
        if (!target->coherence.HasValidContent() ||
            frame.width > uint32_t(id.width) || frame.height > uint32_t(id.height)) return false;
        const auto surface = target->views.color;
        const auto pin = surfaces_.Pin(surface);
        Graphics::Handle handle;
        if (!pin || resources_.RegisterExisting(queue,surfaces_.DescribeSurface(surface),surface,pin,
                                                &handle) != GraphicsResources::Result::Ok) return false;
        const bool ok = queue.RetainResource(handle,&out) == Graphics::Result::Ok;
        queue.ReleaseResource(handle);
        return ok;
    }
    return false;
}
} // namespace title
