// tools/gs_replay/replay_driver.cpp -- the replay loop over the NEW pipeline.
#include "tools/gs_replay/replay_driver.h"

#include <cinttypes>
#include <cstdio>

#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/ir/gsdl_format.h"
#include "engine/gs/hle/ir/job_rec.h"

namespace ps2r {
namespace tools {

using gs::GsdlFormat;
using gs::GsdlReader;
using gs::JobKind;

ReplayDriver::ReplayDriver(const Options& opt, SkipFilter& skip) : opt_(opt), skip_(skip) {}

ReplayDriver::~ReplayDriver() { Teardown(); }

bool ReplayDriver::CreateContext() {
    if (!win_.Create(true, 64, 64, "gs_replay")) {
        std::fprintf(stderr, "[GSREPLAY] SKIP (exit 77): no window\n");
        return false;
    }
    if (!ctx_.Create(win_)) {
        std::fprintf(stderr, "[GSREPLAY] SKIP (exit 77): no GL 4.1 context\n");
        return false;
    }
    ctx_.SetSwapInterval(0);   // measurement mode: never quantize the replay
    if (!be_.Init([this] { win_.Swap(); })) {
        std::fprintf(stderr, "[GSREPLAY] SKIP (exit 77): GlDevice::Init failed\n");
        return false;
    }
    be_.SetWindowSize(win_.Width(), win_.Height());
    std::printf("[GL] %s / %s\n", ctx_.Version(), ctx_.Renderer());
    std::printf("[GL] OpenGL backend initialized (%dx%d)\n", win_.Width(), win_.Height());
    return true;
}

void ReplayDriver::BuildScene() {
    vram_ = std::make_unique<gs::VramImage>();
    profile_ = std::make_unique<title::RendererProfile>();
    scale_ = std::make_unique<gs::ScalePolicy>(gs::ScaleSettings{1}, &profile_->Vram());
    cache_ = std::make_unique<gs::RenderTargetCache>(be_, *vram_, *scale_);
    refresher_ = std::make_unique<gs::TargetRefresher>(be_, *vram_, *cache_);
    resolver_ = std::make_unique<gs::TextureResolver>(be_, *vram_, *cache_, *refresher_, profile_->Vram());
    scanout_ = std::make_unique<gs::ScanoutComposer>(be_, *cache_);
    presenter_ = std::make_unique<gs::Presenter>(*scanout_);
    registry_ = std::make_unique<gs::ClassRegistry>(profile_->ClassRules());
    sequence_ = std::make_unique<gs::SequenceRecipe>(profile_->Sequence());
    gs::DrawPipeline::Deps deps;
    deps.be = &be_;
    deps.vram = vram_.get();
    deps.cache = cache_.get();
    deps.refresher = refresher_.get();
    deps.resolver = resolver_.get();
    deps.scanout = scanout_.get();
    deps.presenter = presenter_.get();
    deps.layout = &profile_->Vram();
    deps.registry = registry_.get();
    deps.sequence = sequence_.get();
    pipe_ = std::make_unique<gs::DrawPipeline>(deps);
    // The render-size fallback starts at the WINDOW's size (OLD Init: SDL_GetWindowSize
    // -> m_width/m_height, opengl_backend.cpp:2846ff): a capture without a SetWindow
    // job renders its non-scanout targets 64 rows tall, exactly as the reference did.
    pipe_->OnSetWindow(win_.Width(), win_.Height(), 0, 0);
    // No DISPFB seen yet: the grab falls back to the most recently drawn target.
    scanout_->SetScanoutFbp(gs::DrawPipeline::kNoFbp);
    profile_->RegisterClassRenderers(*registry_, sequence_.get());
    registry_->WarmUpAll(*pipe_, stdout);
}

void ReplayDriver::Teardown() {
    if (profile_ && be_.IsInitialized()) profile_->ReleaseBackendResources(be_);
    pipe_.reset();
    sequence_.reset();
    registry_.reset();
    presenter_.reset();
    scanout_.reset();
    resolver_.reset();
    refresher_.reset();
    cache_.reset();
    scale_.reset();
    profile_.reset();
    vram_.reset();
    if (be_.IsInitialized()) be_.Shutdown();
    ctx_.Destroy();
    win_.Destroy();
}

int ReplayDriver::Run(const GsdlReader::Recording& rec) {
    if (!(rec.hdr.flags & GsdlFormat::kFlagVerts)) {
        std::fprintf(stderr,
                     "[GSREPLAY] %s was recorded VERTS-OFF (hash-only): no vertex data to replay. Use `census`.\n",
                     opt_.in_path);
        return 1;
    }
    if (!CreateContext()) return 77;
    // The reference tool fingerprinted its PS2_* environment here; this tool reads
    // none, so the fingerprint is the empty set's (the seed).
    std::printf("[GSREPLAY] file=%s version=%u frames=%zu ds=%u env_ps2{n=0 hash=0x%016" PRIx64 "}\n", opt_.in_path,
                rec.hdr.version, rec.frames.size(), opt_.ds_n, kHashSeed);
    if (opt_.out_path && !out_.Open(opt_.out_path, opt_.ds_n)) {
        std::fprintf(stderr, "[GSREPLAY] cannot open %s for write\n", opt_.out_path);
        return 1;
    }
    BuildScene();
    census_ = std::make_unique<Census>(profile_->ClassRules(), rec, opt_.want_cfg);
    replay_hash_ = kHashSeed;
    frames_out_ = 0;
    for (const GsdlReader::Frame& f : rec.frames) {
        if (!opt_.range.Contains(f.vsync_frame)) continue;
        census_->AddFrame();
        uint32_t fdraws = 0;
        uint64_t fverts = 0;
        size_t di = 0, ji = 0;
        for (uint32_t o : f.order) {
            if (o == 1) ReplayDraw(rec, f.draws[di++], &fdraws, &fverts);
            else ReplayJob(f.jobs[ji++]);
        }
        FrameBoundary(f, fdraws, fverts);
    }
    EmitTail();
    if (out_.IsOpen()) {
        out_.Close(frames_out_, replay_hash_, census_->Draws(), census_->Verts());
        std::printf("[GSREPLAY] wrote %s\n", opt_.out_path);
    }
    Teardown();
    return 0;
}

void ReplayDriver::ReplayDraw(const GsdlReader::Recording& rec, const GsdlReader::Draw& d, uint32_t* fdraws,
                              uint64_t* fverts) {
    const gs::DrawRec& x = d.d;
    pipe_->SetSrcFrame(x.src_frame);
    pipe_->SetGfxEpoch(x.vram_epoch);
    const char* name = rec.Name(x.hle_handler);
    bool skipped = false;
    if (!skip_.Empty()) {
        SkipFilter::Fields sf;
        sf.seq = x.seq; sf.prim = x.prim_type; sf.abe = x.alpha_blend; sf.tex = x.tex_enabled;
        sf.tbp = x.tbp; sf.tpsm = x.tpsm; sf.cbp = x.cbp; sf.fbp = x.frame_fbp; sf.fbmsk = x.frame_fbmsk;
        sf.handler = name;
        skipped = skip_.Match(sf);
    }
    if (!skipped) {
        const gs::DrawIr ir = gs::DrawIr::FromDrawRec(x, d.verts.data(), d.verts.size());
        pipe_->Draw(ir, name);
    }
    census_->AddDraw(x);
    (*fdraws)++;
    *fverts += x.vertex_count;
}

void ReplayDriver::ReplayJob(const GsdlReader::Job& jb) {
    const gs::JobRec& j = jb.j;
    pipe_->SetSrcFrame(j.src_frame);
    pipe_->SetGfxEpoch(j.vram_epoch);
    census_->AddJob(j);
    switch (static_cast<JobKind>(j.type)) {
        case JobKind::SetWindow: {
            const int new_w = static_cast<int>(j.arg2 >> 16), new_h = static_cast<int>(j.arg3 >> 16);
            const int cw = static_cast<int>(j.arg2 & 0xFFFF), ch = static_cast<int>(j.arg3 & 0xFFFF);
            const bool resized = new_w > 0 && new_h > 0 && (new_w != pipe_->WindowW() || new_h != pipe_->WindowH());
            pipe_->OnSetWindow(new_w, new_h, cw, ch);
            if (resized) {
                be_.SetWindowSize(new_w, new_h);
                std::printf("[GL] Resized to %dx%d\n", new_w, new_h);
            }
            break;
        }
        case JobKind::SetFrontBuffer:
            pipe_->OnSetFrontBuffer(j.arg1, j.arg2, j.arg3);
            break;
        case JobKind::InvalidateTextures:
            pipe_->OnInvalidate(j.arg1, j.arg2);
            break;
        case JobKind::UploadVRAMTexture:
            if (j.payload_incl && !jb.pixels.empty())
                pipe_->OnUpload(j.arg1, static_cast<int>(j.arg2), static_cast<int>(j.arg3), jb.pixels.data(), j.arg4);
            break;
        case JobKind::SyncAllRTsToVRAM:
            pipe_->OnSyncAll();
            break;
        default:
            break;   // SetClearColor / SetScissor / SetBlendMode / SetDepthTest: display-only or no-ops
    }
}

void ReplayDriver::FrameBoundary(const GsdlReader::Frame& f, uint32_t fdraws, uint64_t fverts) {
    pipe_->SetSrcFrame(f.vsync_frame + 1);
    gs::DrawPipeline::Frame fr;
    pipe_->OnFrameEndGrabOnly(f.vsync.arg1 == 1, &fr);
    int w = 0, h = 0;
    uint64_t full_hash = kNoTargetHash;
    if (fr.grabbed) {
        w = fr.w;
        h = fr.h;
        full_hash = GsdlFormat::Fnv1aFast(fr.rgba.data(), static_cast<size_t>(w) * h * 4);
        if (opt_.dump_dir && opt_.dump_range.Contains(f.vsync_frame)) WritePpm(opt_.dump_dir, f.vsync_frame, fr.rgba, w, h);
    }
    GsrepFile::Downsample(fr.rgba, w, h, opt_.ds_n, ds_);
    const uint64_t ds_hash = GsdlFormat::Fnv1aFast(ds_.data(), ds_.size());
    replay_hash_ = (replay_hash_ * kHashPrime) ^ full_hash;
    replay_hash_ = (replay_hash_ * kHashPrime) ^ ((static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32) |
                                                  static_cast<uint32_t>(h));
    replay_hash_ = (replay_hash_ * kHashPrime) ^ fdraws;
    if (!opt_.quiet)
        std::printf("[GSREPLAY] f=%u vf=%u draws=%u verts=%" PRIu64 " w=%d h=%d fb=0x%016" PRIx64 " ds=0x%016" PRIx64
                    "\n",
                    frames_out_, f.vsync_frame, fdraws, fverts, w, h, full_hash, ds_hash);
    if (out_.IsOpen()) {
        GsrepFrame gf{};
        gf.vsync_frame = f.vsync_frame;
        gf.w = static_cast<uint32_t>(w);
        gf.h = static_cast<uint32_t>(h);
        gf.draws = fdraws;
        gf.verts = fverts;
        gf.full_hash = full_hash;
        gf.ds_hash = ds_hash;
        out_.AppendFrame(gf, ds_);
    }
    frames_out_++;
}

void ReplayDriver::EmitTail() {
    census_->Print("[GSREPLAY]");
    skip_.Emit(stdout);
    std::printf("[GSREPLAY] REPLAY_HASH=0x%016" PRIx64 " frames=%u draws=%" PRIu64 " backend=opengl ds=%u\n",
                replay_hash_, frames_out_, census_->Draws(), opt_.ds_n);
    registry_->EmitCounters(stdout);
    profile_->EmitRendererCounters(stdout);
    pipe_->EmitCounters(stdout);   // this tree's own diagnostics, after the reference-grammar lines
}

void ReplayDriver::WritePpm(const char* dir, uint32_t vf, const std::vector<uint8_t>& rgba, int w, int h) {
    char pp[1024];
    std::snprintf(pp, sizeof(pp), "%s/vf%u.ppm", dir, vf);
    std::FILE* pf = std::fopen(pp, "wb");
    if (!pf) return;
    std::fprintf(pf, "P6\n%d %d\n255\n", w, h);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) std::fwrite(rgba.data() + i * 4, 1, 3, pf);
    std::fclose(pf);
}

}  // namespace tools
}  // namespace ps2r
