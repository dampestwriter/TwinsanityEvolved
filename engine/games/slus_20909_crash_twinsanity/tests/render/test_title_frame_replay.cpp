// tests/render/test_title_frame_replay.cpp -- Render_TitleFrameReplay: one whole frame of
// the PCSX2 GS dump (every host->local upload and every draw, in stream order, exported
// by the dump reader as a `GSF1` stream) replayed through NEW's full lowering --
// TransferEngine -> VramImage -> DrawPipeline (RenderTargetCache / TargetRefresher /
// TextureResolver / class renderers) -> GlDevice -- and grabbed as the console would
// scan it out. The instrument the title-screen investigation needed: seconds per
// frame from the ground truth's own bytes, with `--skip a-b` draw-range ablation.
//
// usage: test_title_frame_replay <stream.bin> [--dump out.ppm] [--skip a-b]... [--only a-b]
//        [--scanout fbp_hex | --last-target] [--repeat n] [--window WxH] [--gate]
// `--gate` (the ctest Render_TitleFrameReplay): the three defect regions of the
// replayed frame against the console's screenshot (games/<title>/verify/baseline/title_screen/
// regions.txt, title_regions.py's rules): LOGO yellow mean within +-8/channel of
// (238,216,16), ORB bright share >= 0.35, OCEAN bright share >= 0.55 -- a regression
// gate for NEW's lowering of this scene (not an equivalence claim across renderers;
// measured at the landing: (241,217,19) / 0.470 / 0.753).
// Exit 77 (SKIP) without a GL context or the stream file.
#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "engine/gs/hle/classrdr/class_registry.h"
#include "engine/gs/hle/classrdr/draw_pipeline.h"
#include "engine/gs/hle/classrdr/sequence_recipe.h"
#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/ir/draw_ir_sink.h"
#include "engine/gs/hle/present/presenter.h"
#include "engine/gs/hle/present/scanout_composer.h"
#include "engine/gs/hle/targets/render_target_cache.h"
#include "engine/gs/hle/targets/scale_policy.h"
#include "engine/gs/hle/targets/target_refresher.h"
#include "engine/gs/hle/texsrc/texture_resolver.h"
#include "engine/gs/vram/vram_image.h"
#include "engine/gs/xfer/transfer_engine.h"
#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"
#include "games/slus_20909_crash_twinsanity/renderer_profile/renderer_profile.h"

using namespace ps2r;

namespace {

struct Range { int a, b; bool Has(int i) const { return i >= a && i <= b; } };

struct Stats { double yr = 0, yg = 0, yb = 0; int yn = 0; double bright = 0; int light = 0; int n = 0; };

// title_regions.py: yellow pixels (R>150, R>G>B, B<120) mean, bright share (max channel >= 230),
// light pixels (R>120, G>120) over a box.
Stats Measure(const std::vector<uint8_t>& rgba, int w, int x0, int y0, int x1, int y1) {
    Stats s;
    int bright = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            const int r = p[0], g = p[1], b = p[2];
            s.n++;
            if (std::max(r, std::max(g, b)) >= 230) bright++;
            if (r > 120 && g > 120) s.light++;
            if (r > 150 && r > g && g > b && b < 120) { s.yr += r; s.yg += g; s.yb += b; s.yn++; }
        }
    if (s.yn) { s.yr /= s.yn; s.yg /= s.yn; s.yb /= s.yn; }
    s.bright = s.n ? double(bright) / s.n : 0.0;
    return s;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(static_cast<size_t>(n));
    const size_t got = std::fread(out->data(), 1, out->size(), f);
    std::fclose(f);
    return got == out->size();
}

// Little-endian stream cursor (plain pointer; the file is the only type here).
uint32_t RdU32(const uint8_t*& p) { uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; }
uint64_t RdU64(const uint8_t*& p) { uint64_t v; std::memcpy(&v, p, 8); p += 8; return v; }

// GS register bit layouts (GS User's Manual 3.4.x) -> the DrawIr fields.
void TexFromRegs(uint64_t tex0, uint64_t tex1, uint64_t clamp, uint64_t texa, uint64_t mip1, uint64_t mip2,
                 gs::TexReadView* v) {
    v->tbp = static_cast<uint32_t>(tex0 & 0x3FFF) * 256u;
    v->tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3F) * 64u;
    v->psm = static_cast<uint32_t>((tex0 >> 20) & 0x3F);
    v->tw = static_cast<uint32_t>((tex0 >> 26) & 0xF);
    v->th = static_cast<uint32_t>((tex0 >> 30) & 0xF);
    v->tcc = static_cast<uint32_t>((tex0 >> 34) & 1);
    v->tfx = static_cast<uint32_t>((tex0 >> 35) & 3);
    v->cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFF) * 256u;
    v->cpsm = static_cast<uint32_t>((tex0 >> 51) & 0xF);
    v->csm = static_cast<uint32_t>((tex0 >> 55) & 1);
    v->csa = static_cast<uint32_t>((tex0 >> 56) & 0x1F);
    v->cld = static_cast<uint32_t>((tex0 >> 61) & 7);
    v->lcm = tex1 & 1;
    v->mxl = static_cast<uint8_t>((tex1 >> 2) & 7);
    v->mmag = (tex1 >> 5) & 1;
    v->mmin = static_cast<uint8_t>((tex1 >> 6) & 7);
    v->l = static_cast<uint8_t>((tex1 >> 19) & 3);
    const uint16_t k12 = static_cast<uint16_t>((tex1 >> 32) & 0xFFF);
    v->k = static_cast<int16_t>(k12 & 0x800 ? static_cast<int>(k12) - 0x1000 : static_cast<int>(k12));
    v->wms = static_cast<uint8_t>(clamp & 3);
    v->wmt = static_cast<uint8_t>((clamp >> 2) & 3);
    v->minu = static_cast<uint16_t>((clamp >> 4) & 0x3FF);
    v->maxu = static_cast<uint16_t>((clamp >> 14) & 0x3FF);
    v->minv = static_cast<uint16_t>((clamp >> 24) & 0x3FF);
    v->maxv = static_cast<uint16_t>((clamp >> 34) & 0x3FF);
    v->texa_ta0 = static_cast<uint8_t>(texa & 0xFF);
    v->texa_aem = (texa >> 15) & 1;
    v->texa_ta1 = static_cast<uint8_t>((texa >> 32) & 0xFF);
    for (int i = 0; i < 3; i++) {
        v->mip_tbp[i] = static_cast<uint32_t>((mip1 >> (20 * i)) & 0x3FFF) * 256u;
        v->mip_tbw[i] = static_cast<uint32_t>((mip1 >> (20 * i + 14)) & 0x3F) * 64u;
        v->mip_tbp[3 + i] = static_cast<uint32_t>((mip2 >> (20 * i)) & 0x3FFF) * 256u;
        v->mip_tbw[3 + i] = static_cast<uint32_t>((mip2 >> (20 * i + 14)) & 0x3F) * 64u;
    }
}

// The sink that applies the transfer engine's jobs to the pipeline as the queue consumer
// does (a POD holder: the file's only type with members is nested here).
struct SinkRig {
    // The transfer engine's jobs, applied to the pipeline as the queue consumer does.
    struct PipeSink final : public gs::DrawIrSink {
        gs::DrawPipeline* pipe = nullptr;
        gs::TransferEngine* xfer = nullptr;
        void OnDraw(const gs::DrawIr&) override {}
        void OnFrame(const gs::JobRec&) override {}
        void OnJob(const gs::JobRec& j) override {
            switch (static_cast<gs::JobKind>(j.type)) {
                case gs::JobKind::InvalidateTextures: pipe->OnInvalidate(j.arg1, j.arg2); break;
                case gs::JobKind::UploadVRAMTexture: {
                    const std::vector<uint8_t>& px = xfer->LastUploadPixels();
                    if (!px.empty() && px.size() == j.payload_bytes)
                        pipe->OnUpload(j.arg1, static_cast<int>(j.arg2), static_cast<int>(j.arg3), px.data(), j.arg4);
                    break;
                }
                default: break;
            }
        }
    };
};

// One pass's knobs (a POD; the stream walk below reads it).
struct ReplayOptions {
    std::vector<Range> skips;               // --skip a-b: draws not lowered
    Range only{-1, -1};                     // --only a-b: the draws lowered (the rest skipped)
    bool have_only = false;
    bool uploads_first = false;             // every upload lands before any draw is lowered (a late consumer)
    uint32_t upload_filter = 0xFFFFFFFFu;   // --upload-dbp hex: apply only the uploads into that base
    int upload_skip_first = 0;              // --upload-skip-first n: drop the first n uploads into the filtered base
};

struct ReplayCounts { int uploads = 0, draws = 0, drawn = 0, skipped = 0; bool bad_record = false; };

// The GSF1 record of one draw -> the DrawIr the queue consumer would hand the pipeline
// (GS register bit layouts, GS User's Manual 3.4.x). `rp` is left after the record.
bool DrawRecordToIr(const uint8_t*& rp, const uint8_t* rend, uint32_t seq, uint32_t src_frame, gs::DrawIr* ir, int* index) {
    *index = static_cast<int>(RdU32(rp));
    const uint32_t attr = RdU32(rp), use_uv = RdU32(rp), cx = RdU32(rp);
    const uint64_t tex0 = RdU64(rp), tex1 = RdU64(rp), clamp = RdU64(rp), alpha = RdU64(rp), test = RdU64(rp);
    const uint64_t frame = RdU64(rp), zbuf = RdU64(rp), xyoff = RdU64(rp), scis = RdU64(rp), fba = RdU64(rp);
    const uint64_t mip1 = RdU64(rp), mip2 = RdU64(rp), texa = RdU64(rp);
    const uint32_t colclamp = RdU32(rp), pabe = RdU32(rp), nverts = RdU32(rp);
    if (static_cast<size_t>(rend - rp) < static_cast<size_t>(nverts) * sizeof(gs::VertexRec)) return false;
    ir->seq = seq;
    ir->src_frame = src_frame;
    ir->prim_type = static_cast<uint8_t>(attr & 7);
    ir->gouraud = (attr >> 3) & 1;
    ir->tex_enabled = (attr >> 4) & 1;
    ir->fog = (attr >> 5) & 1;
    ir->alpha_blend = (attr >> 6) & 1;
    ir->use_fst = use_uv != 0;
    TexFromRegs(tex0, tex1, clamp, texa, mip1, mip2, &ir->tex);
    ir->tex.fst = use_uv != 0;
    ir->raster.frame.fbp = static_cast<uint32_t>(frame & 0x1FF) * 8192u;
    ir->raster.frame.fbw = static_cast<uint32_t>((frame >> 16) & 0x3F) * 64u;
    ir->raster.frame.psm = static_cast<uint8_t>((frame >> 24) & 0x3F);
    ir->raster.frame.fbmsk = static_cast<uint32_t>(frame >> 32);
    ir->raster.zbuf.zbp = static_cast<uint32_t>(zbuf & 0x1FF) * 8192u;
    ir->raster.zbuf.psm = static_cast<uint8_t>((zbuf >> 24) & 0xF);
    ir->raster.zbuf.zmsk = (zbuf >> 32) & 1;
    ir->raster.test.ate = test & 1; ir->raster.test.atst = (test >> 1) & 7; ir->raster.test.aref = (test >> 4) & 0xFF;
    ir->raster.test.afail = (test >> 12) & 3; ir->raster.test.date = (test >> 14) & 1; ir->raster.test.datm = (test >> 15) & 1;
    ir->raster.test.zte = (test >> 16) & 1; ir->raster.test.ztst = (test >> 17) & 3;
    ir->raster.alpha.a = alpha & 3; ir->raster.alpha.b = (alpha >> 2) & 3; ir->raster.alpha.c = (alpha >> 4) & 3;
    ir->raster.alpha.d = (alpha >> 6) & 3; ir->raster.alpha.fix = (alpha >> 32) & 0xFF;
    ir->raster.scissor.x0 = scis & 0x7FF; ir->raster.scissor.x1 = (scis >> 16) & 0x7FF;
    ir->raster.scissor.y0 = (scis >> 32) & 0x7FF; ir->raster.scissor.y1 = (scis >> 48) & 0x7FF;
    ir->raster.colclamp = colclamp != 0; ir->raster.pabe = pabe != 0; ir->raster.fba = fba & 1;
    ir->ctx = static_cast<uint8_t>(cx);
    ir->off_x = static_cast<float>(xyoff & 0xFFFF) / 16.f; ir->off_y = static_cast<float>((xyoff >> 32) & 0xFFFF) / 16.f;
    ir->verts.resize(nverts);
    std::memcpy(ir->verts.data(), rp, static_cast<size_t>(nverts) * sizeof(gs::VertexRec));
    rp += static_cast<size_t>(nverts) * sizeof(gs::VertexRec);
    ir->vertex_count = nverts;
    return true;
}

// One walk of the GSF1 stream in record order: uploads through the transfer engine
// (BITBLTBUF/TRXPOS/TRXREG/TRXDIR + IMAGE data, settled), draws through the pipeline.
ReplayCounts ReplayPass(const std::vector<uint8_t>& stream, const ReplayOptions& opt, gs::DrawPipeline& pipe,
                        gs::TransferEngine& xfer, gs::VramImage& vram, uint32_t src_frame, uint32_t* seq) {
    ReplayCounts n;
    const uint8_t* rp = stream.data() + 8;
    const uint8_t* const rend = stream.data() + stream.size();
    auto ok = [&](size_t k) { return static_cast<size_t>(rend - rp) >= k; };
    uint32_t nrec; std::memcpy(&nrec, stream.data() + 4, 4);
    int upload_skip_first = opt.upload_skip_first;
    std::vector<gs::DrawIr> deferred;
    for (uint32_t r = 0; r < nrec && ok(4); r++) {
        const uint32_t type = RdU32(rp);
        if (type == 1) {
            const uint32_t dbp = RdU32(rp), dbw = RdU32(rp), dpsm = RdU32(rp), dsax = RdU32(rp), dsay = RdU32(rp);
            const uint32_t rrw = RdU32(rp), rrh = RdU32(rp), k = RdU32(rp);
            if (!ok(k)) break;
            if (opt.upload_filter != 0xFFFFFFFFu && dbp != opt.upload_filter) { rp += k; continue; }
            if (upload_skip_first > 0) { upload_skip_first--; rp += k; continue; }
            xfer.WriteRegister(0x50, ((uint64_t)(dbp / 256) << 32) | ((uint64_t)(dbw / 64) << 48) | ((uint64_t)dpsm << 56));
            xfer.WriteRegister(0x51, ((uint64_t)dsax << 32) | ((uint64_t)dsay << 48));
            xfer.WriteRegister(0x52, (uint64_t)rrw | ((uint64_t)rrh << 32));
            xfer.WriteRegister(0x53, 0);
            xfer.WriteImage(rp, k);
            vram.SettleWrites();
            rp += k;
            n.uploads++;
        } else if (type == 2) {
            gs::DrawIr ir;
            int index = 0;
            if (!DrawRecordToIr(rp, rend, ++*seq, src_frame, &ir, &index)) break;
            n.draws++;
            bool skip = opt.have_only && !opt.only.Has(index);
            for (const Range& s : opt.skips) if (s.Has(index)) skip = true;
            if (skip) { n.skipped++; continue; }
            if (opt.uploads_first) { deferred.push_back(std::move(ir)); continue; }
            pipe.SetSrcFrame(src_frame);
            pipe.SetGfxEpoch(ir.seq);
            pipe.Draw(ir, nullptr);
            n.drawn++;
        } else {
            std::printf("bad record type %u at record %u\n", type, r);
            n.bad_record = true;
            break;
        }
    }
    for (const gs::DrawIr& ir : deferred) {
        pipe.SetSrcFrame(src_frame);
        pipe.SetGfxEpoch(ir.seq);
        pipe.Draw(ir, nullptr);
        n.drawn++;
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_title_frame_replay <stream.bin> [--dump out.ppm] [--skip a-b] [--only a-b] [--scanout hex | --last-target] [--repeat n] [--gate]\n");
        return 2;
    }
    const char* dump = nullptr;
    ReplayOptions opt;
    bool last_target = false, gate = false;
    uint32_t scanout_fbp = 0;
    int win_w = 512, win_h = 448;
    int repeat = 1;   // --repeat n: the stream n times with a frame boundary between passes
                      // (pass 2's first draws see pass 1's targets -- a display copy at the
                      // frame's head then carries content, as it does live)
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--upload-dbp") && i + 1 < argc) opt.upload_filter = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "--upload-skip-first") && i + 1 < argc) opt.upload_skip_first = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!std::strcmp(argv[i], "--skip") && i + 1 < argc) { Range r; std::sscanf(argv[++i], "%d-%d", &r.a, &r.b); opt.skips.push_back(r); }
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) { std::sscanf(argv[++i], "%d-%d", &opt.only.a, &opt.only.b); opt.have_only = true; }
        else if (!std::strcmp(argv[i], "--scanout") && i + 1 < argc) scanout_fbp = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "--last-target")) last_target = true;
        else if (!std::strcmp(argv[i], "--gate")) gate = true;
        else if (!std::strcmp(argv[i], "--uploads-first")) opt.uploads_first = true;
        else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--window") && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &win_w, &win_h);
    }
    std::vector<uint8_t> stream;
    if (!ReadFile(argv[1], &stream) || stream.size() < 8 || std::memcmp(stream.data(), "GSF1", 4)) {
        std::printf("test_title_frame_replay: stream %s missing -> SKIP (77)\n", argv[1]);
        return 77;
    }
    platform::Window win;
    platform::GlContext ctx;
    if (!win.Create(true, win_w, win_h, "test_title_frame_replay") || !ctx.Create(win)) {
        std::printf("test_title_frame_replay: no GL context -> SKIP (77)\n");
        return 77;
    }
    ctx.SetSwapInterval(0);
    render::GlDevice be;
    if (!be.Init([&] { win.Swap(); })) { std::printf("GlDevice::Init failed -> SKIP (77)\n"); return 77; }
    be.SetWindowSize(win_w, win_h);

    gs::VramImage vram;
    auto profile = std::make_unique<title::RendererProfile>();
    gs::ScalePolicy scale{gs::ScaleSettings{1}, &profile->Vram()};
    gs::RenderTargetCache cache{be, vram, scale};
    gs::TargetRefresher refresher{be, vram, cache};
    gs::TextureResolver resolver{be, vram, cache, refresher, profile->Vram()};
    gs::ScanoutComposer scanout{be, cache};
    gs::Presenter presenter{scanout};
    gs::ClassRegistry registry{profile->ClassRules()};
    gs::SequenceRecipe sequence{profile->Sequence()};
    gs::DrawPipeline::Deps deps;
    deps.be = &be; deps.vram = &vram; deps.cache = &cache; deps.refresher = &refresher; deps.resolver = &resolver;
    deps.scanout = &scanout; deps.presenter = &presenter; deps.layout = &profile->Vram();
    deps.registry = &registry; deps.sequence = &sequence;
    int rc = 0;
    {
        gs::DrawPipeline pipe(deps);
        pipe.OnSetWindow(win_w, win_h, 512, 448);
        pipe.SetLiveSnapshot(true);
        if (last_target) scanout.SetScanoutFbp(gs::DrawPipeline::kNoFbp);
        else pipe.OnSetFrontBuffer(scanout_fbp, 512, 0x02);   // DISPFB: FBP 0, FBW 512, PSMCT16 (the dump's priv regs)
        profile->RegisterClassRenderers(registry, &sequence);
        registry.WarmUpAll(pipe, nullptr);
        gs::TransferEngine xfer(&vram);
        SinkRig::PipeSink sink;
        sink.pipe = &pipe;
        sink.xfer = &xfer;
        xfer.SetSink(&sink);
        uint32_t seq = 0;
        uint32_t src_frame = 1;
        xfer.SetJobContext(&seq, &src_frame);

        ReplayCounts n;
        gs::DrawPipeline::Frame fr;
        for (int pass = 0; pass < repeat; pass++) {
            src_frame = static_cast<uint32_t>(pass + 1);
            n = ReplayPass(stream, opt, pipe, xfer, vram, src_frame, &seq);
            if (n.bad_record) { rc = 1; break; }
            pipe.OnFrameEndGrabOnly(true, &fr);
            if (repeat > 1)
                std::printf("pass %d: drawn=%d; grab=%d source=%d fbp=%#x\n", pass + 1, n.drawn, fr.grabbed ? 1 : 0,
                            static_cast<int>(fr.source), fr.source_fbp);
        }
        std::printf("replayed: uploads=%d draws=%d drawn=%d skipped=%d; grab=%d %dx%d source=%d fbp=%#x\n", n.uploads, n.draws,
                    n.drawn, n.skipped, fr.grabbed ? 1 : 0, fr.w, fr.h, static_cast<int>(fr.source), fr.source_fbp);
        const gs::TextureResolver::Census& tc = resolver.Stats();
        std::printf("resolver: lookups=%llu l1=%llu l2=%llu decodes=%llu\n", (unsigned long long)tc.lookups,
                    (unsigned long long)tc.l1_hits, (unsigned long long)tc.l2_hits, (unsigned long long)tc.decodes);
        for (const gs::DrawRouteTrace::Line& L : pipe.RouteTrace().LastFrame())
            if (L.tbp == 0x3c0000 || L.tbp == 0x3b0000)
                std::printf("route seq=%u tbp=%#x cbp=%#x owner=%u fmt=%u drew=%d sw=%d\n", L.seq, L.tbp, L.cbp, L.owner_kind, L.fmt_case, L.drew ? 1 : 0, L.sw_blend ? 1 : 0);
        if (dump && fr.grabbed) {
            FILE* f = std::fopen(dump, "wb");
            if (f) {
                std::fprintf(f, "P6\n%d %d\n255\n", fr.w, fr.h);
                for (size_t i = 0; i < fr.rgba.size(); i += 4) std::fwrite(&fr.rgba[i], 1, 3, f);
                std::fclose(f);
                std::printf("frame -> %s\n", dump);
            }
        }
        if (!fr.grabbed) rc = 1;
        if (fr.grabbed && fr.w == 512 && fr.h == 448) {
            const Stats orb = Measure(fr.rgba, fr.w, 196, 150, 316, 290);
            const Stats logo = Measure(fr.rgba, fr.w, 104, 110, 416, 236);
            const Stats ocean = Measure(fr.rgba, fr.w, 0, 318, 512, 388);
            std::printf("regions: ORB bright=%.3f light=%d | LOGO yellow=(%.0f,%.0f,%.0f) n=%d bright=%.3f | OCEAN bright=%.3f light=%d\n",
                        orb.bright, orb.light, logo.yr, logo.yg, logo.yb, logo.yn, logo.bright, ocean.bright, ocean.light);
            std::printf("pcsx2:   ORB bright=0.417 light=5602 | LOGO yellow=(238,216,16) n=20501 bright=0.605 | OCEAN bright=0.659 light=2261\n");
            if (gate) {
                int fails = 0;
                auto req = [&](bool ok, const char* what) { if (!ok) { fails++; std::printf("FAIL gate: %s\n", what); } };
                req(std::fabs(logo.yr - 238) <= 8 && std::fabs(logo.yg - 216) <= 8 && std::fabs(logo.yb - 16) <= 8,
                    "LOGO yellow mean within +-8/channel of the console's (238,216,16)");
                req(logo.yn >= 15000, "LOGO yellow pixel count >= 15000");
                req(orb.bright >= 0.35, "ORB bright share >= 0.35");
                req(ocean.bright >= 0.55, "OCEAN bright share >= 0.55");
                std::printf("test_title_frame_replay gate: %s\n", fails ? "FAIL" : "PASS");
                if (fails) rc = 1;
            }
        } else if (gate) {
            rc = 1;
        }
        profile->ReleaseBackendResources(be);
    }
    profile.reset();
    be.Shutdown();
    return rc;
}
