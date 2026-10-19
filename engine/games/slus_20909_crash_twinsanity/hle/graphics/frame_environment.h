#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// Observes complete source-level frame operations. Values describe the ordered
// frame's intent, not the GS model's current registers or packet contents.
// The supported flat UI intent draws full scanlines with ordinary Mix blending.
// These are native rendering choices, NOT assertions about CSR reset values.
// MaterialState still declines material variants outside that contract.
class FrameEnvironment {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;

    static constexpr uint32_t kInitDisplayAddress = 0x0019ade0;
    static constexpr uint32_t kWriteFrameHeadAddress = 0x0019aaa8;
    static constexpr uint32_t kSetUpFrameAddress = 0x0019f1c8;
    static constexpr uint32_t kFinishSkyAddress = 0x001b9f30;
    static constexpr uint32_t kBeginShadowsAddress = 0x001c9048;
    static constexpr uint32_t kEndShadowsAddress = 0x001c9ac0;
    static constexpr uint32_t kLinkAddress = 0x0017ed50;
    static constexpr uint32_t kResetGraphAddress = 0x002c1bd8;
    static constexpr size_t kBindingCount = 8;
    static constexpr uint32_t kFrameBufferPage = 0x0030a77c;
    static constexpr uint32_t kDrawPixelBytes = 0x00309834;
    static constexpr uint32_t kFrameBuckets = 0x00323658;

    // Completed source frame setup, copied before a producer queues work.
    // Keys are byte offsets only for the temporary shared-scene adapter.
    // source_frame counts primary bucket submissions since reset, starting
    // at zero (the GraphicsService ordinal). epoch is a monotonic SOURCE
    // setup generation, never a VRAM/upload epoch; it survives reset.
    struct SceneFrame {
        uint32_t color_buffer = 0, depth_buffer = 0;
        uint32_t width = 0, height = 0, source_frame = 0;
        uint64_t epoch = 0;
    };
    struct Snapshot {
        SceneFrame scene, submitted;
        uint32_t next_frame = 0;
        uint64_t source_epoch = 0;
        bool frame = false, dither_known = false, sky_known = false;
        bool scene_valid = false, submitted_valid = false;
    };
    explicit FrameEnvironment(MaterialState::Read source = {});

    enum class Status { NoFrameBoundary, IncompleteOperation, Ready };
    struct Evidence {
        bool frame = false;
        bool dither_matrix = false;
        bool shadow_open = false;
        uint64_t completed_frame_boundaries = 0;
        uint64_t complete_display_initializations = 0;
        uint64_t interior_entries = 0;
    };

    // Capture after SDK installation. Link composes with GraphicsService's
    // wrapper in either order: it invalidates this producer epoch before the
    // original link, after all accepted Strip templates were captured.
    // Captures every body before installation; failed installation rolls back.
    bool Install(const Capture& capture, const Installer& installer);

    // Exact callback for GraphicsService/GraphicsOutput. Failure leaves out
    // unchanged. Only use this environment with MaterialState's flat UI case.
    // No per-pixel blend gating or odd/even scanline suppression is part of
    // that title interface. Fog is inactive; its colour is canonical zero.
    bool Read(MaterialState::Environment& out) const;
    // Every family bucket of TR renderer.h:431 that draws into the frame's
    // scene: opaque 2/3, screen effects 4, blended 6-19, decals 20, shadows
    // 21, particles 22, distortion 23, UI 24, fonts 26, movies 27. Bucket 0
    // is CaptureSky's; 1/5/25 have no family and refuse. This reads no RAM:
    // the complete tuple was captured at SetUpFrame/FinishSky entry and
    // published only on successful completion. Bucket 21 alone is admitted
    // while the shadow bracket (BeginShadows..EndShadows, TR
    // chunkscenery.cpp:1617) is open, including from a producer wrapped
    // inside either bracket body; every other bucket requires the bracket
    // closed and no nested source operation. False leaves out unchanged.
    bool CaptureScene(uint32_t bucket, SceneFrame& out) const;
    static bool SceneBucket(uint32_t bucket);
    // Effect buckets (TR renderer.h:431: screen effects 4, shadows 21,
    // distortion 23): the frame's completed scene tuple under CaptureScene's
    // own admission (21 inside the open shadow bracket) PLUS the frame-wide
    // environment the source head/sky established (TR frame.cpp:104-106,130:
    // colour clamp on, dither off, the dither matrix; full scanlines and
    // ordinary alpha are the title frame intent). Fog is NOT frame state: TR
    // materials.cpp sets FOGCOL per shader, so the effect owner captures fog
    // from its own material; the tuple's fog bytes are 0 and are not a
    // fog-off claim. Never a UI-bucket substitute: 24/26 stay with CaptureUi.
    bool CaptureEffects(uint32_t bucket, SceneFrame& frame, MaterialState::Environment& environment) const;
    static bool EffectBucket(uint32_t bucket);
    // Material buckets (TR renderer.h:431: opaque 2/3, the blended 6-19): the
    // frame's completed scene tuple under CaptureScene's admission (the shadow
    // bracket closed, no nested operation) PLUS the frame-wide environment the
    // source head/sky established -- the only writers of COLCLAMP/DTHE/DIMX/
    // PABE/SCANMSK in the title are frame.cpp/sky.cpp (and the shadow bracket,
    // which restores clamp before EndShadows): every material bucket inherits
    // the same environment (the owned mesh bucket's hook). Fog is
    // the material's own (TR materials.cpp sets FOGCOL per shader; the type-01
    // envelope has it off): the tuple's fog bytes are 0. Never a UI/effect/sky
    // substitute: 0, 4, 21, 23, 24, 26, 27 refuse here.
    bool CaptureMesh(uint32_t bucket, SceneFrame& frame, MaterialState::Environment& environment) const;
    static bool MeshBucket(uint32_t bucket);
    // Last successfully linked primary source frame, captured before Link
    // invalidates the producer epoch. Presentation never rereads later RAM.
    bool CaptureSubmittedScene(SceneFrame& out) const;
    // Whether any complete primary Link (frame-buckets SubmitBuckets) has run
    // since the last quiescent reset/restore: the Link ordinal is non-zero.
    // False means no scene has ever been linked into the drawn buffer.
    bool Linked() const;
    // Only inside the complete SetUpFrame producer, under this outer observer.
    // Captured before invocation; published as a completed frame only on success.
    bool CaptureSetup(SceneFrame& out) const;
    // StartSkyBuffer's complete producer preflight. Sky replaces SetUpFrame
    // in DrawRendererScene. Requires a completed source environment since
    // reset; captures current software target globals and the prospective
    // FinishSky generation without publishing a completed scene.
    bool CaptureSky(SceneFrame& out, MaterialState::Environment& environment) const;
    Status CurrentStatus() const;
    Evidence Observed() const;
    // Source facts only, captured after producer/consumer quiescence. An
    // unfinished operation or shadow boundary refuses rather than inventing
    // an inherited environment. Restore never executes a source body.
    bool CaptureState(Snapshot&, std::string* error = nullptr) const;
    bool RestoreState(const Snapshot&, std::string* error = nullptr);

    // Call only after the producer and render queues are quiescent, on both
    // reset and before explicit source-state publication during restore.
    // A restored interior continuation never establishes a source boundary.
    // Lifetime counters survive; per-frame/source knowledge is cleared.
    void ResetAfterQuiescence();

private:
    enum class Boundary {
        InitDisplay, WriteFrameHead, SetUpFrame, FinishSky,
        BeginShadows, EndShadows, Link, ResetGraph
    };
    std::array<Body, kBindingCount> originals_;
    MaterialState::Read source_;
    SceneFrame scene_;
    SceneFrame submitted_scene_;
    SceneFrame setup_scene_;
    bool setup_valid_ = false;
    uint64_t setup_invalidations_ = 0;
    bool submitted_valid_ = false;
    bool scene_valid_ = false;
    bool sky_environment_known_ = false;
    uint32_t source_frame_ = 0;
    uint64_t source_epoch_ = 0, invalidations_ = 0;
    bool installed_ = false;
    uint32_t active_operations_ = 0;
    uint32_t shadow_bracket_operations_ = 0;
    MaterialState::Environment environment_;
    Evidence evidence_;
    void Run(Boundary boundary, CpuContext& context, uint32_t entry);
    void EstablishFlatUi();
    void InvalidateFrame();
    void InvalidateAll();
    bool ReadSceneSource(Boundary boundary, uint32_t argument, SceneFrame& out) const;
};

}  // namespace title
