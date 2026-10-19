#pragma once

#include <atomic>
#include <map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_source.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"

namespace title {

// Complete source shadow producer. CPU matrix changes happen at begin/end;
// geometry closes with the source instance blocks, at its first material slot.
class ShadowPass {
public:
    using Frame = FrameEnvironment::SceneFrame;
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    static constexpr uint32_t kClose = 0x0019c638, kForget = 0x001c0410;
    // Undelivered reservations the pass holds at most; a bracket past it is
    // refused to the model with this detail word (producer kBegin, ordering).
    static constexpr size_t kPendingLimit = 64;
    static constexpr uint32_t kLimitDetail = 0x4000;
    struct Batch {
        const Frame frame;
        explicit Batch(Frame value) : frame(value) {}
        bool Ready() const;
        bool Complete() const;
        bool Failed() const;
        const std::vector<ps2r::gs::DrawIr>& Draws() const;
        // Ordered consumer, exactly once. Failure is sticky; an early consumer
        // cannot treat an unfinished producer as an empty successful pass.
        bool Finish(bool success) const;
    private:
        friend class ShadowPass;
        enum class State { Building, Ready, Complete, Failed };
        mutable std::atomic<State> state_{State::Building};
        std::vector<ps2r::gs::DrawIr> draws_;
        uint32_t meshes_ = 0;   // accepted default-mesh calls while this bracket was open
    };
    struct Hooks {
        bool enabled = false;
        MaterialState::Read read;
        // All source writes are atomic; false/throw changes no RAM.
        std::function<bool(const std::vector<ShadowSource::Write>&)> write;
        // The source-frame owner, before this pass's matrix mutation.
        std::function<bool(Frame&)> frame;
        // Reserve bucket 21's exact source predecessor now. The consumer must
        // check Ready, execute EffectsScene::Shadows(frame, Draws), then Finish.
        // No consumption before source CloseInstanceBlocks; no RAM reads later.
        // False/throw must leave the output queue unchanged.
        std::function<bool(std::shared_ptr<const Batch>)> reserve;
        // Attribution of refusals (ShadowLoads conversion refusals and this
        // pass's default-mesh refusals; empty = uncounted). A mesh refusal's
        // producer is ShadowSource::kMesh, its detail ShadowSource::Detail().
        GraphicsRefusals::Sink refuse = {};
    };
    struct Count { uint64_t calls = 0, replaced = 0, model_calls = 0, interior = 0; };
    // The last default-mesh call this pass refused to the model (section 8a):
    // the declined check and the source words it saw, for attribution.
    struct MeshRefusal {
        ShadowSource::Result result = ShadowSource::Result::Ok;
        ShadowSource::MeshDetail detail;
    };
    struct Counters {
        Count begin, mesh, end;
        uint64_t instances = 0, blocks = 0, groups = 0, completed = 0, faults = 0;
        // Owned brackets closed with no accepted mesh: the consumer publishes
        // NOTHING for them (no clear, no composite). The source's empty
        // StartShadows/ApplyShadows pair only costs fill; it changes no pixel.
        uint64_t empty = 0;
        // Selected brackets whose source frame/matrix capture or source-position
        // reservation failed BEFORE any guest write: the whole bracket (begin, meshes, end)
        // ran the model bodies and is counted in their model_calls. A refusal
        // is attributable census, never a fault.
        uint64_t refused = 0;
        // Of those, brackets refused because kPendingLimit reservations were
        // still undelivered by the ordered output (the output is stalled; the
        // model draws this frame's shadows). Attributed: kBegin / kLimitDetail.
        uint64_t limit_refusals = 0;
        // Default-mesh calls inside an owned bracket whose input this owner could
        // not check (no owned asset for the mesh's model, a foreign shape/material,
        // unreadable words). The owned begin already bypassed the source's
        // half-size target set-up, so the model body of ONE mesh would draw its
        // volume strips into the scene (the s3 white streak): the call is taken
        // (mesh.replaced), NOT drawn by either arm, and attributed through
        // Hooks::refuse. The bracket and its other meshes are unchanged.
        uint64_t refused_meshes = 0;
        MeshRefusal last_mesh_refusal;
        // What the empty brackets ARE (the s4 question: 1573 of 2703 passes).
        // Per Close (one source frame): how many owned brackets the frame had
        // (the source runs one DrawShadows per renderer view), and each empty
        // bracket's class: no_mesh = not one default-mesh call reached it (no
        // caster drawn in view: Crash off-screen, a menu/camera frame, a chunk
        // whose entry list was clipped away); redirected = its meshes were
        // accepted but belong to a material another bracket of the same frame
        // opened first (source material order: the FIRST pass's slot draws them).
        uint64_t frames_closed = 0, frames_one_bracket = 0, frames_many_brackets = 0;
        uint64_t empty_no_mesh = 0, empty_redirected = 0;
        // Frames whose EVERY bracket was empty (nothing composited that frame),
        // as inclusive source-frame ranges; bounded, the tail counted.
        struct Range { uint32_t first = 0, last = 0; };
        std::vector<Range> empty_frames;
        uint64_t empty_frames_total = 0, empty_frames_unlisted = 0;
        uint32_t last_published_frame = 0;
    };
    static constexpr size_t kEmptyFrameRanges = 48;
    ShadowPass(ShadowAssets&, Hooks);
    bool Install(const Capture&, const Installer&);
    void Run(uint32_t boundary, CpuContext&, uint32_t entry);
    Counters Counts() const;
    // `unlinked_recording_open`: the ordered output's capture-time settle
    // (Graphics INTERFACES section 12) admits this frame's CLOSED pass whose
    // batches are Ready behind their bucket-21 predecessor but not yet Linked
    // (the title Links after SyncV); a closed boundary (false) needs them
    // Complete and forgotten.
    bool CanCheckpoint(bool unlinked_recording_open = false) const;
    // Explicit abandoned-frame/reset after the root output fence, never an
    // absent-payload restore operation. Does not reset ShadowAssets.
    void ResetAfterQuiescence();
private:
    struct Block {
        uint32_t object = 0, material = 0;
        ShadowAssets::Lease asset;
        std::vector<ShadowGeometry::View> views;
        // Source InstanceBlock::size in quadwords (10 or 11 per instance): the
        // block closes when size + PlacedRoom reaches its capacity.
        uint32_t quadwords = 0;
        bool eye = false;
    };
    struct Material {
        std::shared_ptr<Batch> batch;
        std::vector<std::shared_ptr<Block>> blocks;
    };
    ShadowAssets& assets_;
    Hooks hooks_;
    std::map<uint32_t, Body> originals_;
    std::vector<std::shared_ptr<Batch>> batches_, pending_;
    std::vector<uint32_t> material_order_;
    std::map<uint32_t, Material> materials_;
    std::map<uint32_t, std::shared_ptr<Block>> active_;
    std::shared_ptr<Batch> open_;
    uint32_t source_view_ = 0;
    Counters counts_;
    bool installed_ = false, closed_ = false, faulted_ = false, refusing_ = false;
    // False: refused before any reservation or write (the caller runs the model).
    bool Begin();
    // False: this default-mesh call's input is not checked/owned; nothing was
    // recorded for it and nothing draws it (attributed, see refused_meshes).
    bool Mesh(const CpuContext&);
    void End();
    void Close();
    void Forget();
    void Collect();
    [[noreturn]] void Fault(const char*);
};

}  // namespace title
