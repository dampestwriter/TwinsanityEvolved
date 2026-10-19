#pragma once

#include "games/slus_20909_crash_twinsanity/composition/graphics_producer_draws.h"
#include "games/slus_20909_crash_twinsanity/composition/mesh_producer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_selection.h"

class MainMemory;
namespace ps2r::app {
class GraphicsOutput;
class VideoStack;

// Source producer composition. Install TextureBindings first, this owner
// second, GraphicsOutput::Install last (its FinishSky observer must be outer).
// Root owns immutable toggles, texture persistence, Movie lifecycle/picture/
// present services, and keeps every owner alive until dispatch and output stop.
class GraphicsProducers {
public:
    using Frame = GraphicsProducerDraws::Frame;
    using Environment = ::title::MaterialState::Environment;
    using Body = ::title::TextureBindings::Body;
    using Capture = ::title::TextureBindings::Capture;
    using Installer = ::title::TextureBindings::Installer;
    struct Options {
        bool sprites = false, text = false, sky = false, movie = false;
        bool synthesize_tail = false;
        bool textures = false;   // textures at load: every record converts; the slot upload binds it
        // A4: the texture pack directory (`<data dir>/textures/<video.texture_pack>`,
        // empty = none) and the --dump-textures directory (empty = none).
        std::string texture_pack_dir, dump_textures_dir;
        // The owned mesh bucket (MeshProducer) -- the certified
        // type-01 rigid/placed materials' instances built from the owned model
        // assets and recorded at their bucket's slot; requires `textures`.
        bool meshes = false;
    };
    // The 16 KB scratchpad at 0x70000000: the title's main thread runs on it
    // (TR src/platform/ps2/entry.cpp:83), so Stream/matrix arguments the
    // producers read are stack objects there. Optional: without the window a
    // scratchpad extent is InvalidMemory and the producer refuses.
    struct Window { uint8_t* data; size_t size; }; // {} = no window
    // The root's extra physical region above main RAM ([kArenaBase, +size)):
    // under hle.meta_arena the title's compacting handle heap (GetDiskManager's
    // singleton) keeps its 0x20-byte block descriptors there, and the sky
    // owner's load-boundary conversion (SkyAssets::ModelLoaded/TextureLoaded)
    // finds a model's bytes through those nodes. Optional: without the window
    // a node there is InvalidMemory, the load is declined and every selected
    // sky pass refuses `missing_resource` (ModelMissing, detail 4).
    static constexpr uint32_t kArenaBase = 0x02000000;
    struct Counters {
        ::title::SpriteService::Counters sprites;
        ::title::TextService::Counters text;
        ::title::SkyPass::Counters sky;
        MeshProducer::Counters meshes;
        uint64_t flush_calls = 0, flush_replaced = 0, flush_model_calls = 0;
        uint64_t recorded = 0, completed = 0, pending = 0, failures = 0;
        size_t text_materials = 0;
        uint64_t cancelled = 0;
        uint64_t refused_recordings = 0; // ordered output refused a reservation before any commit
        uint64_t refused_slots = 0;      // delivered groups the owned scene refused at the slot (nothing drawn)
    };
    GraphicsProducers(MainMemory&, GraphicsOutput&, VideoStack&, ::title::TextureAssets&,
                      ::title::TextureBindings&, ::title::TextureSelection&, Options, Window scratchpad = {},
                      Window arena = {});
    // The same composition seams exposed for GL-free isolated checks. Work
    // must run on the ordered backend owner and retain its captures until done.
    struct Output {
        std::function<bool(uint32_t, Frame&, Environment&)> ui;
        std::function<bool(Frame&, Environment&)> sky;
        std::function<bool(Frame&)> movie;
        // The material buckets' frame + environment (GraphicsOutput::
        // CaptureMesh). Optional: without it every mesh collection declines.
        std::function<bool(uint32_t, Frame&, Environment&)> mesh;
        std::function<bool(uint32_t, std::function<bool()>)> record;
        std::function<bool(std::function<bool()>)> immediate;
        // The capture-time settle of the ordered output (GraphicsOutput::
        // SettleOwnedForCapture): true when the boundary is settled with an
        // OPEN recording of at least `pending` owned items -- this owner's
        // pending tickets are then that frame's recorded, unlinked groups
        // (TR renderer.cpp:436-440) and the boundary is admitted. Optional:
        // without it any pending ticket refuses the capture.
        std::function<bool(uint64_t pending)> settled;
        // Optional process-lifetime ledger: the Sprite/Text/Sky producers'
        // refusals and faults are attributed into it (never owned here).
        ::title::GraphicsRefusals* refusals = nullptr;
    };
    GraphicsProducers(MainMemory&, GraphicsProducerDraws&, ::title::TextureBindings&,
                      ::title::TextureSelection&, Options, Output, Window scratchpad = {}, Window arena = {});
    ~GraphicsProducers();
    GraphicsProducers(const GraphicsProducers&) = delete;
    GraphicsProducers& operator=(const GraphicsProducers&) = delete;
    bool Install(const Capture&, const Installer&);
    // Image leases include the movie-only guarded sampled view.
    // Root passes these to MovieGraphics' OutputServices constructor. Its
    // actual original_calls/Census remain on that MovieGraphics instance.
    ::title::MovieGraphics::OutputServices MovieOutput();
    Counters Counts() const;
    bool CanCheckpoint(std::string* reason = nullptr) const;
    bool CaptureSkyAssets(::title::SkyAssets::Snapshot&, std::string* reason = nullptr) const;
    bool RestoreSkyAssets(const ::title::SkyAssets::Snapshot&, std::string* reason = nullptr);
    // The owned mesh bucket's MaterialResourceRead receipts (MeshProducer::
    // Capture/RestoreReceipts; the MSHR section). Without `meshes` the capture
    // is empty and a restore is a no-op.
    bool CaptureMeshReceipts(std::vector<uint8_t>& out) const;
    bool RestoreMeshReceipts(const std::vector<uint8_t>& payload, const ::title::MaterialState::Read& incoming,
                             std::string* reason = nullptr);
    // Quiescent owner reset only. Pending/faulted selected work is refused;
    // there is no automatic replay, missing-asset import, or queue recovery.
    void ResetAfterQuiescence();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ps2r::app
