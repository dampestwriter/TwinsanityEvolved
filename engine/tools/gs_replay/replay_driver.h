// tools/gs_replay/replay_driver.h -- ReplayDriver: the corpus replay of one
// `.gsdl` recording through the NEW GS HLE layers (DrawPipeline over the GL
// backend) with the reference tool's exact frame loop, hash chain and outputs.
//
// World: a hidden 64x64 window + GL context + GlDevice; a zero VramImage that is
// never committed (the recording carries texture HASHES, not bytes, so every
// VRAM decode reads zero plus the replay's own deposits -- deterministic and
// identical in both legs of any replay comparison); the title's RendererProfile
// (VRAM layout, class rules, the seven class renderers); RenderTargetCache /
// TargetRefresher / TextureResolver / ScanoutComposer / Presenter / ClassRegistry /
// SequenceRecipe / DrawPipeline at scale 1.
//
// Frame loop, per recorded frame in stream order: a draw sets the stream stamps
// (src_frame, vram_epoch) from the record, lifts DrawIr::FromDrawRec and runs
// DrawPipeline::Draw with the recording's HLE name (the --skip filter counts a
// matching draw without replaying it); a job sets the same stamps and maps to
// the pipeline's job entry (SetWindow -> content dims + window size; SetFrontBuffer
// -> scanout base + DISPFB; InvalidateTextures -> OnInvalidate; UploadVRAMTexture
// with its payload -> OnUpload; SyncAllRTsToVRAM -> OnSyncAll; the rest are
// no-ops). The kFrame record: src_frame = vsync_frame + 1, the EE frame mark
// when arg1 == 1, the reader-band present flush, the display grab (full physical
// rect, RGBA8, bottom-up) -> full_hash = Fnv1aFast over the bytes (the "no target"
// sentinel 0x9E3779B97F4A7C15 with w = h = 0 otherwise), the ds_n x ds_n box
// downsample -> ds_hash, the three-step chain
//   replay_hash = (replay_hash * P) ^ full_hash; (.. * P) ^ (w << 32 | h); (.. * P) ^ draws
// (P = 1099511628211, seed 1469598103934665603), the per-frame line, the .gsrep
// frame, then the swap (no display quad: the reference replay never ran the
// scanout composite). The scanout base starts unset (the reference's sentinel):
// a capture without a DISPFB job grabs the most recently drawn target.
// After the frames: the census block, the skip counts, the REPLAY_HASH line, the
// class-registry routing table and the renderer counters in the reference order.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/gs/hle/classrdr/class_registry.h"
#include "engine/gs/hle/classrdr/draw_pipeline.h"
#include "engine/gs/hle/classrdr/sequence_recipe.h"
#include "engine/gs/hle/present/presenter.h"
#include "engine/gs/hle/present/scanout_composer.h"
#include "engine/gs/hle/targets/render_target_cache.h"
#include "engine/gs/hle/targets/scale_policy.h"
#include "engine/gs/hle/targets/target_refresher.h"
#include "engine/gs/hle/texsrc/texture_resolver.h"
#include "engine/gs/vram/vram_image.h"
#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"
#include PS2R_TITLE_RENDERER_PROFILE_H   // the configured title's renderer_profile/renderer_profile.h (CMake)
#include "tests/lib/gsdl_reader.h"
#include "tools/gs_replay/census.h"
#include "tools/gs_replay/frame_range.h"
#include "tools/gs_replay/gsrep_file.h"
#include "tools/gs_replay/skip_filter.h"

namespace ps2r {
namespace tools {

class ReplayDriver {
public:
    struct Options {
        const char* in_path = nullptr;
        const char* out_path = nullptr;    // .gsrep artifact (optional)
        const char* dump_dir = nullptr;    // --dump-ppm: vf%u.ppm of every grabbed frame in dump_range
        uint32_t ds_n = 32;
        FrameRange range;                  // --frames: the frames REPLAYED
        FrameRange dump_range;             // --dump-frames: the frames DUMPED (the whole replay still runs)
        bool quiet = false;
        bool want_cfg = false;
    };
    static constexpr uint64_t kHashSeed = 1469598103934665603ull;
    static constexpr uint64_t kHashPrime = 1099511628211ull;
    static constexpr uint64_t kNoTargetHash = 0x9E3779B97F4A7C15ull;

    ReplayDriver(const Options& opt, SkipFilter& skip);
    ~ReplayDriver();

    // Exit code: 0 ok, 1 = VERTS-OFF recording or unwritable --out, 77 = no GL.
    int Run(const gs::GsdlReader::Recording& rec);

    uint64_t ReplayHash() const { return replay_hash_; }
    uint32_t FramesOut() const { return frames_out_; }

private:
    bool CreateContext();
    void BuildScene();
    void Teardown();
    void ReplayDraw(const gs::GsdlReader::Recording& rec, const gs::GsdlReader::Draw& d, uint32_t* fdraws,
                    uint64_t* fverts);
    void ReplayJob(const gs::GsdlReader::Job& jb);
    void FrameBoundary(const gs::GsdlReader::Frame& f, uint32_t fdraws, uint64_t fverts);
    void EmitTail();
    static void WritePpm(const char* dir, uint32_t vf, const std::vector<uint8_t>& rgba, int w, int h);

    Options opt_;
    SkipFilter& skip_;
    platform::Window win_;
    platform::GlContext ctx_;
    render::GlDevice be_;
    std::unique_ptr<gs::VramImage> vram_;
    std::unique_ptr<title::RendererProfile> profile_;
    std::unique_ptr<gs::ScalePolicy> scale_;
    std::unique_ptr<gs::RenderTargetCache> cache_;
    std::unique_ptr<gs::TargetRefresher> refresher_;
    std::unique_ptr<gs::TextureResolver> resolver_;
    std::unique_ptr<gs::ScanoutComposer> scanout_;
    std::unique_ptr<gs::Presenter> presenter_;
    std::unique_ptr<gs::ClassRegistry> registry_;
    std::unique_ptr<gs::SequenceRecipe> sequence_;
    std::unique_ptr<gs::DrawPipeline> pipe_;
    std::unique_ptr<Census> census_;
    GsrepFile out_;
    uint64_t replay_hash_ = kHashSeed;
    uint32_t frames_out_ = 0;
    std::vector<uint8_t> ds_;
};

}  // namespace tools
}  // namespace ps2r
