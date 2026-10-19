// tests/render/test_title_logo_draw.cpp -- Render_TitleLogoDrawPin: ONE title-screen
// draw from the PCSX2 GS dump, replayed from the dump's own bytes through NEW's full
// lowering (TransferEngine upload -> TextureResolver decode -> GlDevice::Draw on the
// real pipeline over a GL context) and measured against the console's screenshot.
//
// Draw [167] of frame 1 (the logo letters): PSMT8 32x32 atlas at 0x3c0000 with its
// CSM1 CT32 CLUT at 0x3c0400 (one 64x32 CT32 page upload), TEX0 MODULATE / TCC 1,
// TEX1 MMAG LINEAR, MMIN LINEAR_MIPMAP_NEAREST, MXL 2, LCM 0, K = -4.3125, CLAMP
// REPEAT, PRIM tristrip IIP TME STQ, ABE 0, TEST atst GEQUAL aref 4 afail FB_ONLY,
// ZTST GEQUAL, FRAME 0x70000 CT32 fbw 512, vertex colour 0xFF (2x modulate).
// GS rule under test (GS User's Manual 3.4.12): with LCM = 0 the LOD is
// (log2(1/|Q|) << L) + K -- a function of Q and the authored K only; with this
// draw's Q in [0.33, 0.37] every pixel has LOD <= -2.8 < 0, so the GS samples
// level 0 with the MMAG filter everywhere. The console's letter pixels are the
// level-0 texel x2 (yellow mean (238,216,16) over the LOGO region of the dump's
// screenshot); a lowering that selects a coarser level (a footprint-derived LOD
// floor) blends the atlas's neighbouring flat colours into the letters and lowers
// the yellow pixels' share. The pin measures the yellow-pixel mean and the bright
// share over the draw's bbox as games/<title>/verify/baseline/title_screen/title_regions.py does.
// Exit 77 (SKIP) without a GL context or the baseline files.
#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "engine/gs/hle/classrdr/class_registry.h"
#include "engine/gs/hle/classrdr/draw_pipeline.h"
#include "engine/gs/hle/classrdr/sequence_recipe.h"
#include "engine/gs/hle/ir/draw_ir.h"
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

static int g_checks = 0, g_fail = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

namespace {

constexpr int kW = 512, kH = 448;

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

// TEX0 / TEX1 / CLAMP / TEXA bit layouts (GS User's Manual 3.4.4 / 3.4.5 / 3.4.7 / 3.4.6).
gs::TexReadView ViewFromRegs(uint64_t tex0, uint64_t tex1, uint64_t clamp, uint64_t texa) {
    gs::TexReadView v;
    v.tbp = static_cast<uint32_t>(tex0 & 0x3FFF) * 256u;
    v.tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3F) * 64u;
    v.psm = static_cast<uint32_t>((tex0 >> 20) & 0x3F);
    v.tw = static_cast<uint32_t>((tex0 >> 26) & 0xF);
    v.th = static_cast<uint32_t>((tex0 >> 30) & 0xF);
    v.tcc = static_cast<uint32_t>((tex0 >> 34) & 1);
    v.tfx = static_cast<uint32_t>((tex0 >> 35) & 3);
    v.cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFF) * 256u;
    v.cpsm = static_cast<uint32_t>((tex0 >> 51) & 0xF);
    v.csm = static_cast<uint32_t>((tex0 >> 55) & 1);
    v.csa = static_cast<uint32_t>((tex0 >> 56) & 0x1F);
    v.cld = static_cast<uint32_t>((tex0 >> 61) & 7);
    v.lcm = tex1 & 1;
    v.mxl = static_cast<uint8_t>((tex1 >> 2) & 7);
    v.mmag = (tex1 >> 5) & 1;
    v.mmin = static_cast<uint8_t>((tex1 >> 6) & 7);
    v.l = static_cast<uint8_t>((tex1 >> 19) & 3);
    const uint16_t k12 = static_cast<uint16_t>((tex1 >> 32) & 0xFFF);
    v.k = static_cast<int16_t>(k12 & 0x800 ? static_cast<int>(k12) - 0x1000 : static_cast<int>(k12));
    // MIPTBP1 = 0x003c0007c0a07c08 (the dump's [167]): level 1 at 0x3c0800, level 2 at 0x3c0a00, TBW 64 --
    // the authored chain inside the uploaded page, as the live run decodes it (TexresTrace f2519: levels=2).
    v.mip_tbp[0] = 0x3c0800u; v.mip_tbw[0] = 64u;
    v.mip_tbp[1] = 0x3c0a00u; v.mip_tbw[1] = 64u;
    v.wms = static_cast<uint8_t>(clamp & 3);
    v.wmt = static_cast<uint8_t>((clamp >> 2) & 3);
    v.texa_ta0 = static_cast<uint8_t>(texa & 0xFF);
    v.texa_aem = (texa >> 15) & 1;
    v.texa_ta1 = static_cast<uint8_t>((texa >> 32) & 0xFF);
    return v;
}

// The dump's registers for draw [167] (frame 1) -> a DrawIr; verts from the VertexRec file.
gs::DrawIr LogoDraw(const std::vector<uint8_t>& verts_bin) {
    gs::DrawIr ir;
    ir.prim_type = 4;          // PRIM 0x4 tristrip; PRMODE 0x18: IIP TME
    ir.gouraud = true;
    ir.tex_enabled = true;
    ir.use_fst = false;        // STQ
    ir.alpha_blend = false;    // ABE 0
    ir.tex = ViewFromRegs(0x2007808555307c00ull, 0xffffffbb00000128ull, 0ull, 0x8000000000ull);
    ir.tex.fst = false;
    // FRAME 0x0000000000080038: FBP 0x38*8192 = 0x70000, FBW 8*64 = 512, PSM CT32, FBMSK 0.
    ir.raster.frame.fbp = 0x70000; ir.raster.frame.fbw = 512; ir.raster.frame.psm = 0; ir.raster.frame.fbmsk = 0;
    // ZBUF 0x00000000300000a8: ZBP 0xa8*8192 = 0x150000, PSM Z32, ZMSK 0.
    ir.raster.zbuf.zbp = 0x150000; ir.raster.zbuf.psm = 0; ir.raster.zbuf.zmsk = false;
    // TEST 0x51048: ATE 0 ATST 4 AREF 4 AFAIL 1 ZTE 1 ZTST 2.
    ir.raster.test.ate = false; ir.raster.test.atst = 4; ir.raster.test.aref = 4; ir.raster.test.afail = 1;
    ir.raster.test.zte = true; ir.raster.test.ztst = 2;
    ir.raster.alpha.a = 2; ir.raster.alpha.b = 0; ir.raster.alpha.c = 0; ir.raster.alpha.d = 1;   // 0x42, unused (ABE 0)
    ir.raster.scissor = {0, 0, 511, 447};
    ir.raster.colclamp = true;
    ir.ctx = 0;
    ir.off_x = 1792.f; ir.off_y = 1824.f;   // XYOFFSET 0x7000 / 0x7200 (12.4)
    const size_t n = verts_bin.size() / sizeof(gs::VertexRec);
    ir.verts.resize(n);
    std::memcpy(ir.verts.data(), verts_bin.data(), n * sizeof(gs::VertexRec));
    ir.vertex_count = static_cast<uint32_t>(n);
    return ir;
}

struct Stats { double yr = 0, yg = 0, yb = 0; int yn = 0; double bright = 0; int n = 0; };

// title_regions.py's statistics over a box: yellow pixels (R>150, R>G>B, B<120) mean,
// bright share (max channel >= 230).
Stats Measure(const std::vector<uint8_t>& rgba, int w, int x0, int y0, int x1, int y1) {
    Stats s;
    int bright = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            const int r = p[0], g = p[1], b = p[2];
            s.n++;
            if (std::max(r, std::max(g, b)) >= 230) bright++;
            if (r > 150 && r > g && g > b && b < 120) { s.yr += r; s.yg += g; s.yb += b; s.yn++; }
        }
    if (s.yn) { s.yr /= s.yn; s.yg /= s.yn; s.yb /= s.yn; }
    s.bright = s.n ? double(bright) / s.n : 0.0;
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    const char* dump = nullptr;
    size_t split = 0;   // --split <verts>: the list as separate draws of <verts> (rounded to triangles)
    for (int i = 2; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--dump")) dump = argv[i + 1];
        if (!std::strcmp(argv[i], "--split")) split = static_cast<size_t>(std::atoi(argv[i + 1])) / 3 * 3;
    }
    std::vector<uint8_t> page, verts;
    if (!ReadFile(dir + "/logo_atlas_page_f1_xfer243.bin", &page) || page.size() != 8192 ||
        !ReadFile(dir + "/logo_draw167_f1_verts.bin", &verts) || verts.size() % sizeof(gs::VertexRec) != 0) {
        std::printf("test_title_logo_draw: baseline files missing under %s -> SKIP (77)\n", dir.c_str());
        return 77;
    }
    platform::Window win;
    platform::GlContext ctx;
    if (!win.Create(true, kW, kH, "test_title_logo_draw") || !ctx.Create(win)) {
        std::printf("test_title_logo_draw: no GL context -> SKIP (77)\n");
        return 77;
    }
    render::GlDevice be;
    CHECK(be.Init([&] { win.Swap(); }), "Init");
    be.SetWindowSize(kW, kH);

    // The pipeline exactly as the replay driver / host wires it.
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
    {
        gs::DrawPipeline pipe(deps);
        pipe.OnSetWindow(kW, kH, kW, kH);
        pipe.SetLiveSnapshot(true);   // the host wiring: the VRAM mirror is live (not a payload-less replay)
        scanout.SetScanoutFbp(gs::DrawPipeline::kNoFbp);
        profile->RegisterClassRenderers(registry, &sequence);
        registry.WarmUpAll(pipe, nullptr);

        // The dump's upload before [167]: BITBLTBUF dbp 0x3c0000 dbw 64 CT32, TRXREG 64x32, host->local.
        gs::TransferEngine xfer(&vram);
        xfer.WriteRegister(0x50, ((uint64_t)(0x3c0000 / 256) << 32) | (1ull << 48));
        xfer.WriteRegister(0x51, 0);
        xfer.WriteRegister(0x52, 64ull | (32ull << 32));
        xfer.WriteRegister(0x53, 0);
        xfer.WriteImage(page.data(), page.size());
        vram.SettleWrites();
        pipe.OnInvalidate(0x3c0000, 8192);

        // A black cleared target at FRAME 0x70000 (ABE 0: the result is the modulated texel).
        gs::DrawIr ir = LogoDraw(verts);
        pipe.SetSrcFrame(1);
        pipe.SetGfxEpoch(1);
        bool native = false;
        if (split >= 3 && split < ir.verts.size()) {
            // The live run's shape: the same triangles as consecutive draws of `split` vertices.
            size_t draws = 0;
            for (size_t off = 0; off < ir.verts.size(); off += split) {
                gs::DrawIr part = ir;
                part.verts.assign(ir.verts.begin() + static_cast<long>(off),
                                  ir.verts.begin() + static_cast<long>(std::min(off + split, ir.verts.size())));
                native = pipe.Draw(part, "vs_textured_tristrip");
                ++draws;
            }
            std::printf("draw [167]: %zu verts as %zu draws of %zu, native=%d\n", ir.verts.size(), draws, split, native ? 1 : 0);
        } else {
            native = pipe.Draw(ir, "vs_textured_tristrip");
            std::printf("draw [167]: %zu verts, native=%d\n", ir.verts.size(), native ? 1 : 0);
        }
        gs::DrawPipeline::Frame fr;
        pipe.OnFrameEndGrabOnly(true, &fr);
        const std::vector<gs::DrawRouteTrace::Line>& lines = pipe.RouteTrace().LastFrame();
        if (!lines.empty()) {
            const gs::DrawRouteTrace::Line& L = lines.back();
            std::printf("route: owner=%u fmt=%u drew=%d sw_blend=%d tbp=%#x cbp=%#x\n", L.owner_kind, L.fmt_case,
                        L.drew ? 1 : 0, L.sw_blend ? 1 : 0, L.tbp, L.cbp);
        }
        CHECK(fr.grabbed && fr.w == kW && fr.h == kH, "the drawn target grabbed at 512x448");
        if (fr.grabbed) {
            // The draw's bbox (dump: x 97.6..428.2, y 115.1..367.0) -- the same yellow / bright rules
            // as title_regions.py over the LOGO region (104,110)-(416,236) of the screenshot.
            const Stats s = Measure(fr.rgba, fr.w, 104, 110, 416, 236);
            std::printf("NEW [167] LOGO box: yellow mean=(%.0f,%.0f,%.0f) n=%d bright=%.3f\n", s.yr, s.yg, s.yb, s.yn, s.bright);
            std::printf("PCSX2 screenshot LOGO: yellow mean=(238,216,16) n=20501 bright=0.605 (regions.txt)\n");
            // The pin grabs the draw's PSMCT32 target directly: (250,230,14) n=17734 bright=0.432 for
            // the draw alone on black. The console's screenshot shows the frame through its PSMCT16
            // display (DISPFB PSM 0x02: 5-bit channels, the display copy's quantisation), hence
            // (238,216,16); +-16/channel covers that quantisation (one 5-bit step = 8). The live NEW
            // full frame measured (213,177,65) bright 0.182 over the same box.
            CHECK(s.yn > 15000, "the letters are drawn (yellow pixels present)");
            CHECK(std::fabs(s.yr - 238) <= 16 && std::fabs(s.yg - 216) <= 16 && std::fabs(s.yb - 16) <= 16,
                  "letter yellow mean within +-16/channel of the console's (238,216,16)");
            CHECK(s.bright > 0.40, "bright share: the letters are the 2x-modulated level-0 texels");
            if (dump) {
                FILE* f = std::fopen(dump, "wb");
                if (f) {
                    std::fprintf(f, "P6\n%d %d\n255\n", fr.w, fr.h);
                    for (size_t i = 0; i < fr.rgba.size(); i += 4) std::fwrite(&fr.rgba[i], 1, 3, f);
                    std::fclose(f);
                    std::printf("frame -> %s\n", dump);
                }
            }
        }
        profile->ReleaseBackendResources(be);
    }
    profile.reset();
    be.Shutdown();
    std::printf("test_title_logo_draw: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
