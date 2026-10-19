// tests/gs/classrdr/test_classrdr.cpp -- the class-renderer stage: classifier
// decision tree, registry dispatch + fallback, Accept shapes, the sequence FSM +
// byte arithmetic, the column-shuffle snap, the draw pipeline order, the counter
// grammar, the profile's registration. GL-free (FakeBackend).
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/gs/hle/classrdr/class_classifier.h"
#include "engine/gs/hle/classrdr/class_registry.h"
#include "engine/gs/hle/classrdr/draw_pipeline.h"
#include "engine/gs/format/psm_layout.h"
#include "engine/gs/hle/classrdr/sequence_recipe.h"
#include "engine/gs/hle/classrdr/stencil_composite_renderer.h"
#include "engine/gs/hle/classrdr/texture_shuffle_snap.h"
#include "engine/gs/hle/present/presenter.h"
#include "engine/gs/hle/present/scanout_composer.h"
#include "engine/gs/hle/targets/render_target_cache.h"
#include "engine/gs/hle/targets/scale_policy.h"
#include "engine/gs/hle/targets/target_refresher.h"
#include "engine/gs/hle/texsrc/texture_resolver.h"
#include "engine/gs/vram/vram_image.h"
#include "games/slus_20909_crash_twinsanity/renderer_profile/renderer_profile.h"
#include "tests/lib/fake_backend.h"

using namespace ps2r;
using namespace ps2r::gs;
using C = ClassClassifier;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fails++;                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);       \
        }                                                                   \
    } while (0)

// The process-wide profile serves CONST title data only (rules, shapes, VRAM
// layout, sequence constants); it never registers renderers -- those are owned
// by each World's own profile (declared last, destroyed first) so no renderer
// outlives the backend / pipeline / registry it caches.
static const title::RendererProfile& Profile() {
    static const title::RendererProfile p;
    return p;
}

static void AddVert(DrawIr& ir, float x, float y, uint16_t u = 0, uint16_t v = 0, uint32_t rgba = 0x80808080u) {
    VertexRec r{};
    r.x = x; r.y = y; r.z = 0; r.q = 1.0f;
    r.uv_u = u; r.uv_v = v;
    r.r = rgba & 0xFF; r.g = (rgba >> 8) & 0xFF; r.b = (rgba >> 16) & 0xFF; r.a = (rgba >> 24) & 0xFF;
    ir.verts.push_back(r);
}
static void AddQuad(DrawIr& ir, float x0, float y0, float x1, float y1, uint32_t rgba = 0x80808080u) {
    AddVert(ir, x0, y0, 0, 0, rgba); AddVert(ir, x1, y0, 0, 0, rgba); AddVert(ir, x0, y1, 0, 0, rgba);
    AddVert(ir, x1, y0, 0, 0, rgba); AddVert(ir, x1, y1, 0, 0, rgba); AddVert(ir, x0, y1, 0, 0, rgba);
}
static DrawIr Raw() {
    DrawIr ir;
    ir.prim_type = 3;   // triangle
    ir.raster.frame.fbp = 0x70000; ir.raster.frame.fbw = 512; ir.raster.frame.psm = 0;
    ir.raster.scissor = DrawIr::Scissor{0, 0, 511, 447};
    AddVert(ir, 0, 0); AddVert(ir, 8, 0); AddVert(ir, 0, 8);
    return ir;
}

// ---- classify (OLD test_gs_classrdr decision tree) -------------------------
static void TestClassify() {
    const C::Rules& R = Profile().ClassRules();
    DrawIr b = Raw();
    b.identity.hle_handler = 0xDEAD;
    struct Row { const char* nm; C::Class want; };
    const Row rows[] = {
        {"vs_skinned_lit_strip", C::kSkinned}, {"vs_family_tristrip", C::kWorldTristrip},
        {"vs_textured_tristrip", C::kWorldTristrip}, {"vs_lit_tristrip", C::kWorldTristrip},
        {"vs_poscolor_strip", C::kParticlesUntex}, {"vs_indexed_displace", C::kIndexedDisplace},
        {"vs_sprite_text", C::kSpritesHud}, {"vs_mystery_program", C::kHleOther}};
    for (const Row& r : rows) CHECK(C::Classify(R, b, r.nm) == r.want, r.nm);
    CHECK(C::Classify(R, b, nullptr) == C::kHleOther, "stamped, no name -> hle-other");
    b.raster.test.date = true;
    CHECK(C::Classify(R, b, "vs_family_tristrip") == C::kWorldTristrip, "stamp beats raw shape");
    // Raw shapes.
    b = Raw(); b.raster.test.date = true;
    CHECK(C::Classify(R, b, nullptr) == C::kStencilComposite, "DATE -> composite");
    b = Raw(); b.raster.frame.psm = 10;
    CHECK(C::Classify(R, b, nullptr) == C::kStencilComposite, "CT16S frame -> composite");
    b = Raw(); b.tex_enabled = true; b.tex.psm = 27;
    CHECK(C::Classify(R, b, nullptr) == C::kStencilComposite, "T8H read -> composite");
    b.tex_enabled = false;
    CHECK(C::Classify(R, b, nullptr) == C::kRawOther, "T8H with TME off is raw-other");
    b = Raw(); b.verts.clear(); b.prim_type = 6; b.tex_enabled = true; AddQuad(b, 0, 0, 256, 128);
    CHECK(C::Classify(R, b, nullptr) == C::kRawBlitFullscreen, "256x128 textured sprite = blit");
    b.verts.clear(); AddQuad(b, 0, 0, 255.5f, 128);
    CHECK(C::Classify(R, b, nullptr) == C::kRawSpritesHud, "just under the width threshold");
    b.verts.clear(); AddQuad(b, 0, 0, 256, 127.5f);
    CHECK(C::Classify(R, b, nullptr) == C::kRawSpritesHud, "just under the height threshold");
    b.verts.clear(); b.tex_enabled = false; AddQuad(b, 0, 0, 640, 448);
    CHECK(C::Classify(R, b, nullptr) == C::kRawSpritesHud, "untextured big sprite = HUD");
    b.verts.clear(); b.tex_enabled = true; AddQuad(b, 100, 100, 356, 228);
    CHECK(C::Classify(R, b, nullptr) == C::kRawBlitFullscreen, "extent, not position");
    CHECK(C::ClassifyFields(R, 0, nullptr, false, 0, true, 0, 6, 256.0f, 128.0f) == C::kRawBlitFullscreen,
          "ClassifyFields == Classify");
    b = Raw();
    CHECK(C::Classify(R, b, nullptr) == C::kRawOther, "plain raw triangle");
    // Names: the census strings.
    CHECK(!std::strcmp(C::ClassName(C::kWorldTristrip), "world-tristrip"), "name 0");
    CHECK(!std::strcmp(C::ClassName(C::kSkinned), "skinned"), "name 1");
    CHECK(!std::strcmp(C::ClassName(C::kParticlesUntex), "particles-untex"), "name 2");
    CHECK(!std::strcmp(C::ClassName(C::kIndexedDisplace), "indexed-displace"), "name 3");
    CHECK(!std::strcmp(C::ClassName(C::kSpritesHud), "sprites-HUD"), "name 4");
    CHECK(!std::strcmp(C::ClassName(C::kHleOther), "hle-other"), "name 5");
    CHECK(!std::strcmp(C::ClassName(C::kStencilComposite), "S9-shadow"), "name 6");
    CHECK(!std::strcmp(C::ClassName(C::kRawBlitFullscreen), "raw-blit-fullscreen"), "name 7");
    CHECK(!std::strcmp(C::ClassName(C::kRawSpritesHud), "raw-sprites-HUD"), "name 8");
    CHECK(!std::strcmp(C::ClassName(C::kRawOther), "raw-other"), "name 9");
    // Empty rules: nothing but the sprite / raw split.
    C::Rules none;
    b = Raw(); b.raster.test.date = true; b.raster.frame.psm = 10;
    CHECK(C::Classify(none, b, nullptr) == C::kRawOther, "no composite rules -> raw-other");
}

// ---- a GL-free pipeline world -----------------------------------------------
struct World {
    struct CountingRenderer final : public ClassRenderer {
        bool accept = true;
        int draws = 0, warmups = 0, programs = 1;
        const char* Name() const override { return "counting"; }
        bool Accept(const DrawIr&) const override { return accept; }
        void Draw(DrawPipeline& p, const DrawIr& ir) override { draws++; p.DrawGeneric(ir); }
        int WarmUp(DrawPipeline&) override { warmups++; return programs; }
    };
    struct RecordingMaterializer final : public SequenceMaterializer {
        int calls = 0;
        void Materialize(DrawPipeline&) override { calls++; }
    };
    test::FakeBackend be;
    VramImage vram;
    ScalePolicy scale{ScaleSettings{1}, &Profile().Vram()};
    RenderTargetCache cache{be, vram, scale};
    TargetRefresher refresher{be, vram, cache};
    TextureResolver resolver{be, vram, cache, refresher, Profile().Vram()};
    ScanoutComposer scanout{be, cache};
    Presenter presenter{scanout};
    ClassRegistry registry{Profile().ClassRules()};
    SequenceRecipe sequence{Profile().Sequence()};
    DrawPipeline pipe{DrawPipeline::Deps{&be, &vram, &cache, &refresher, &resolver, &scanout, &presenter,
                                          &Profile().Vram(), &registry, &sequence}};
    title::RendererProfile profile;   // owns the renderers registered into `registry`; dies before `be`
};

// ---- dispatch + fallback (invariant 29) --------------------------------------
static void TestDispatch() {
    World w;
    ClassRegistry& reg = w.registry;
    for (int i = 0; i < C::kClassCount; i++) CHECK(reg.Slot(static_cast<uint8_t>(i)) == nullptr, "empty slot");
    DrawIr raw = Raw();
    DrawIr hle = Raw(); hle.identity.hle_handler = 7;
    CHECK(!w.pipe.Draw(raw, nullptr), "no renderer -> generic");
    CHECK(!w.pipe.Draw(hle, "vs_family_tristrip"), "no renderer -> generic (hle)");
    CHECK(w.be.draws == 2, "both draws reached the backend once each");
    const ClassRegistry::Counters& n = reg.Count();
    CHECK(n.seen[C::kRawOther] == 1 && n.fb_norenderer[C::kRawOther] == 1, "raw-other counted");
    CHECK(n.seen[C::kWorldTristrip] == 1 && n.fb_norenderer[C::kWorldTristrip] == 1, "world counted");
    CHECK(n.native[C::kRawOther] == 0 && n.native[C::kWorldTristrip] == 0, "nothing native");
    CHECK(reg.CurrentClass() == -1, "class slot cleared after the draw");
    // Native routing, declines, disables, registry swap.
    World::CountingRenderer world;
    CHECK(reg.Register(C::kWorldTristrip, &world) == nullptr, "prev empty");
    const int before = w.be.draws;
    CHECK(w.pipe.Draw(hle, "vs_family_tristrip"), "native");
    CHECK(world.draws == 1 && w.be.draws == before + 1, "renderer drew via the generic path once");
    CHECK(n.native[C::kWorldTristrip] == 1 && n.seen[C::kWorldTristrip] == 2, "native counted");
    CHECK(n.verts[C::kWorldTristrip] == 6, "verts accumulate");
    world.accept = false;
    CHECK(!w.pipe.Draw(hle, "vs_family_tristrip"), "declined -> generic");
    CHECK(world.draws == 1 && n.fb_declined[C::kWorldTristrip] == 1 && w.be.draws == before + 2, "fallback drew");
    world.accept = true;
    reg.SetEnabled(C::kWorldTristrip, false);
    CHECK(!w.pipe.Draw(hle, "vs_family_tristrip"), "disabled -> generic");
    CHECK(world.draws == 1 && n.fb_disabled[C::kWorldTristrip] == 1, "disabled counted");
    reg.SetEnabled(C::kWorldTristrip, true);
    CHECK(reg.Register(C::kWorldTristrip, nullptr) == &world, "unregister returns the occupant");
    CHECK(!w.pipe.Draw(hle, "vs_family_tristrip") && world.draws == 1, "cleared slot falls back");
    // The fallback and the native replica issue the SAME backend call (params equal).
    reg.Register(C::kWorldTristrip, &world);
    w.pipe.Draw(hle, "vs_family_tristrip");
    const render::DrawParams pn = w.be.last_params;
    reg.Register(C::kWorldTristrip, nullptr);
    w.pipe.Draw(hle, "vs_family_tristrip");
    const render::DrawParams pg = w.be.last_params;
    CHECK(pn.color.id == pg.color.id && pn.render_w == pg.render_w && pn.render_h == pg.render_h &&
              pn.alt16_route == pg.alt16_route && pn.do_frame_clear == pg.do_frame_clear,
          "native replica == generic backend call");
    // Warm-up.
    World::CountingRenderer a, b; a.programs = 5; b.programs = 3;
    reg.Register(C::kSkinned, &a); reg.Register(C::kSpritesHud, &b);
    CHECK(reg.WarmUpAll(w.pipe, nullptr) == 8 && a.warmups == 1 && b.warmups == 1, "warm-up sums");
    reg.SetEnabled(C::kSpritesHud, false);
    CHECK(reg.WarmUpAll(w.pipe, nullptr) == 5 && a.warmups == 2 && b.warmups == 1, "disabled skipped");
    CHECK(reg.RegisteredCount() == 2, "two registered");
}

// ---- Accept shapes (INV 7.4) -------------------------------------------------
static DrawIr Strip() {
    DrawIr ir = Raw();
    ir.prim_type = 4; ir.use_fst = true; ir.alpha_blend = true;
    ir.raster.alpha = DrawIr::Alpha{0, 2, 2, 1, 0x80}; ir.raster.colclamp = false;
    ir.raster.test.date = true; ir.raster.test.zte = true; ir.raster.test.ztst = 2; ir.raster.zbuf.zmsk = true;
    ir.raster.frame.psm = 0x0A; ir.raster.frame.fbmsk = 0xff000000u; ir.raster.frame.fbp = 0x230000;
    return ir;
}
static DrawIr Skinned() {
    DrawIr ir = Raw();
    ir.prim_type = 4; ir.gouraud = true; ir.raster.colclamp = true; ir.raster.test.zte = true; ir.raster.test.ztst = 2;
    ir.tex_enabled = true; ir.tex.psm = 0x13; ir.tex.tcc = 1; ir.tex.mmag = true; ir.tex.mmin = 4; ir.tex.tbw = 64;
    return ir;
}
static DrawIr Sprite(bool textured) {
    DrawIr ir = Raw(); ir.verts.clear(); AddQuad(ir, 0, 0, 16, 16);
    ir.prim_type = 6; ir.raster.fba = true; ir.raster.test.zte = true; ir.raster.test.ztst = 1; ir.raster.zbuf.zmsk = true;
    ir.raster.frame.fbmsk = 0xff000000u; ir.raster.colclamp = true;
    if (textured) {
        ir.gouraud = true; ir.use_fst = true; ir.alpha_blend = true; ir.raster.alpha = DrawIr::Alpha{0, 1, 0, 1, 0};
        ir.tex_enabled = true; ir.tex.psm = 0x13; ir.tex.tcc = 1; ir.tex.tw = 7; ir.tex.th = 8; ir.tex.tbw = 128;
        ir.tex.wms = 1; ir.tex.wmt = 1; ir.tex.mmag = true; ir.tex.mmin = 1;
    }
    return ir;
}
static DrawIr Particles() {
    DrawIr ir = Raw();
    ir.prim_type = 4; ir.gouraud = true; ir.raster.colclamp = true; ir.raster.test.zte = true; ir.raster.test.ztst = 1;
    ir.tex_enabled = true; ir.tex.psm = 0x13; ir.tex.tcc = 1; ir.tex.mmag = true; ir.tex.mmin = 1;
    return ir;
}
static DrawIr Blit() {
    DrawIr ir = Raw(); ir.verts.clear(); AddQuad(ir, 0, 0, 512, 448);
    ir.prim_type = 6; ir.use_fst = true; ir.raster.colclamp = true; ir.raster.test.zte = true; ir.raster.test.ztst = 2;
    ir.raster.zbuf.zmsk = true; ir.raster.frame.fbmsk = 0xff000000u;
    ir.tex_enabled = true; ir.tex.psm = 0; ir.tex.tfx = 1; ir.tex.tcc = 1; ir.tex.tw = 10; ir.tex.th = 10; ir.tex.tbw = 512;
    return ir;
}

static void TestAccept() {
    const title::RendererProfile::Shapes& S = Profile().Shape();
    // World strip: positive + one-field negatives.
    DrawIr s = Strip();
    CHECK(S.world_strip.Matches(s), "strip accepted");
    { DrawIr n = s; n.prim_type = 5; CHECK(!S.world_strip.Matches(n) && S.composite_fan.Matches(n), "fan is the composite family"); }
    { DrawIr n = s; n.gouraud = true; CHECK(!S.world_strip.Matches(n), "strip: iip"); }
    { DrawIr n = s; n.raster.colclamp = true; CHECK(!S.world_strip.Matches(n), "strip: colclamp"); }
    { DrawIr n = s; n.raster.alpha.d = 2; CHECK(!S.world_strip.Matches(n), "strip: ABCD"); }
    { DrawIr n = s; n.raster.test.datm = true; CHECK(!S.world_strip.Matches(n), "strip: datm"); }
    { DrawIr n = s; n.raster.frame.fbmsk = 0; CHECK(!S.world_strip.Matches(n), "strip: fbmsk"); }
    { DrawIr n = s; n.raster.frame.fbw = 0; CHECK(!S.world_strip.Matches(n), "strip: fbw"); }
    { DrawIr n = s; n.verts.pop_back(); CHECK(!S.world_strip.Matches(n), "strip: n < 3"); }
    // Skinned.
    DrawIr k = Skinned();
    CHECK(S.skinned.Matches(k), "skinned accepted");
    { DrawIr n = k; n.raster.fba = true; CHECK(S.skinned.Matches(n), "skinned: fba free"); }
    { DrawIr n = k; n.raster.test.ate = true; n.raster.test.atst = 5; CHECK(S.skinned.Matches(n), "skinned: ate GEQUAL/KEEP"); }
    { DrawIr n = k; n.raster.test.ate = true; n.raster.test.atst = 6; CHECK(!S.skinned.Matches(n), "skinned: other atst"); }
    { DrawIr n = k; n.tex.mmin = 1; CHECK(!S.skinned.Matches(n), "skinned: mmin"); }
    { DrawIr n = k; n.use_fst = true; CHECK(!S.skinned.Matches(n), "skinned: fst"); }
    { DrawIr n = k; n.tex.psm = 0x14; CHECK(!S.skinned.Matches(n), "skinned: tpsm"); }
    // Sprites.
    DrawIr t = Sprite(true), u = Sprite(false);
    CHECK(S.sprite_common.Matches(t) && S.sprite_textured.Matches(t), "textured sprite");
    CHECK(S.sprite_common.Matches(u) && S.sprite_untextured.Matches(u) && !S.sprite_textured.Matches(u), "untextured sprite");
    { DrawIr n = t; n.raster.fba = false; CHECK(!S.sprite_common.Matches(n), "sprite: fba"); }
    { DrawIr n = t; n.tex.tbw = 64; CHECK(!S.sprite_textured.Matches(n), "sprite: tbw"); }
    { DrawIr n = t; n.tex.tw = 8; CHECK(!S.sprite_textured.Matches(n), "sprite: tw"); }
    { DrawIr n = t; n.raster.test.ztst = 2; CHECK(!S.sprite_common.Matches(n), "sprite: ztst"); }
    // Particles.
    DrawIr p = Particles();
    CHECK(S.particles.Matches(p), "particles abe0");
    { DrawIr n = p; n.alpha_blend = true; n.raster.alpha = DrawIr::Alpha{0, 2, 0, 1, 0}; CHECK(S.particles.Matches(n), "particles 0201"); }
    { DrawIr n = p; n.alpha_blend = true; n.raster.alpha = DrawIr::Alpha{0, 2, 2, 1, 0}; CHECK(!S.particles.Matches(n), "particles: 0221 declines"); }
    { DrawIr n = p; n.raster.zbuf.zmsk = true; CHECK(!S.particles.Matches(n), "particles: zmsk"); }
    { DrawIr n = p; n.tex.wms = 1; CHECK(!S.particles.Matches(n), "particles: wms"); }
    // FMV blit.
    DrawIr f = Blit();
    CHECK(S.fmv_blit.Matches(f), "blit 512");
    { DrawIr n = f; n.tex.tbw = 640; CHECK(S.fmv_blit.Matches(n), "blit 640"); }
    { DrawIr n = f; n.tex.tbw = 1024; CHECK(!S.fmv_blit.Matches(n), "blit: tbw"); }
    { DrawIr n = f; n.tex.tfx = 0; CHECK(!S.fmv_blit.Matches(n), "blit: tfx"); }
    { DrawIr n = f; n.tex.mmag = true; CHECK(!S.fmv_blit.Matches(n), "blit: mmag"); }
    { DrawIr n = f; n.alpha_blend = true; CHECK(!S.fmv_blit.Matches(n), "blit: abe"); }
    // Registered renderers accept exactly these.
    World w;
    CHECK(w.profile.RegisterClassRenderers(w.registry) == 6, "six slots");
    CHECK(w.registry.Slot(C::kWorldTristrip)->Accept(s) && w.registry.Slot(C::kWorldTristrip)->Accept(k), "world: strip + family 2");
    CHECK(w.registry.Slot(C::kSkinned)->Accept(k) && !w.registry.Slot(C::kSkinned)->Accept(s), "skinned slot");
    CHECK(w.registry.Slot(C::kRawOther)->Accept(k) && !w.registry.Slot(C::kRawOther)->Accept(Raw()), "raw-other = raw skin");
    CHECK(w.registry.Slot(C::kSpritesHud)->Accept(t) && w.registry.Slot(C::kSpritesHud)->Accept(u), "sprites slot");
    CHECK(w.registry.Slot(C::kParticlesUntex)->Accept(p), "particles slot");
    CHECK(w.registry.Slot(C::kRawBlitFullscreen)->Accept(f) && !w.registry.Slot(C::kRawBlitFullscreen)->Accept(t), "blit slot");
    // 6F' P6-NATIVE-STALE-UNIFORMS: each registered renderer's generic lowering names
    // the uniforms its reference native arm never writes; the fallback names none.
    auto inherited = [&](DrawIr ir, const char* name) {
        ir.identity.hle_handler = name ? 7u : 0u;   // a name rides an HLE identity; raw draws none
        w.pipe.Draw(ir, name);
        return w.be.last_params.inherited_uniforms;
    };
    CHECK(inherited(k, "vs_family_tristrip") == ClassRenderer::kInheritSkinnedArm, "world family 2 -> skinned arm");
    CHECK(inherited(k, "vs_skinned_lit_strip") == ClassRenderer::kInheritSkinnedArm, "skinned -> skinned arm");
    CHECK(inherited(k, nullptr) == ClassRenderer::kInheritSkinnedArm, "raw skin -> skinned arm");
    CHECK(inherited(t, "vs_sprite_text") == ClassRenderer::kInheritSpriteHudArm, "sprites -> sprite arm");
    CHECK(inherited(p, "vs_poscolor_strip") == ClassRenderer::kInheritParticlesArm, "particles -> particles arm");
    CHECK(inherited(f, nullptr) == ClassRenderer::kInheritFmvBlitArm, "blit -> fmv arm");
    CHECK(inherited(Raw(), nullptr) == 0, "fallback: the full set");
    CHECK((ClassRenderer::kInheritSkinnedArm & render::kInheritCt16Quant) && (ClassRenderer::kInheritSkinnedArm & render::kInheritDither),
          "skinned arm inherits the CT16 quantisation + dither (load_transition vf855)");
    CHECK(!(ClassRenderer::kInheritSpriteHudArm & render::kInheritBlendAbcd), "sprite arm writes blend a..d");
}

// ---- live snapshot + the full-set rule for textured VRAM-decode draws ----------
// Replay (default): a textured read nothing serves is declined NoSnapshot, nothing
// decoded, and the class arm's inherited mask rides the draw. Live: the same draw
// decodes from the VRAM image (a texture reaches the backend) and the draw lowers
// with the FULL uniform set (mask 0). A live target serve or an untextured class
// draw keeps its arm: only the decode route is the reference's specialised program.
static void TestLiveSnapshot() {
    World w;
    w.profile.RegisterClassRenderers(w.registry);
    CHECK(!w.pipe.LiveSnapshot(), "replay by default");
    DrawIr k = Skinned(); k.identity.hle_handler = 7;
    k.tex.tbp = 0x380000; k.tex.tw = 6; k.tex.th = 5; k.tex.tbw = 64;
    const int uploads0 = w.be.uploads;
    w.pipe.Draw(k, "vs_skinned_lit_strip");
    CHECK(w.pipe.LastSource().declined == TextureSource::Declined::NoSnapshot &&
              w.pipe.LastSource().owner.kind == TextureSource::OwnerKind::None,
          "replay: textured VRAM read declined NoSnapshot");
    CHECK(w.be.uploads == uploads0, "replay: nothing decoded");
    CHECK(w.be.last_params.inherited_uniforms == ClassRenderer::kInheritSkinnedArm, "replay: the skinned arm rides");
    w.pipe.SetLiveSnapshot(true);
    CHECK(w.pipe.LiveSnapshot(), "live set");
    w.pipe.Draw(k, "vs_skinned_lit_strip");
    CHECK(w.pipe.LastSource().declined == TextureSource::Declined::None &&
              w.pipe.LastSource().owner.kind == TextureSource::OwnerKind::Decoded,
          "live: the VRAM-decode tier served the read");
    CHECK(w.be.uploads > uploads0 && w.be.Has(w.pipe.LastSource().owner.surface), "live: a decoded texture is bound");
    CHECK(w.be.last_params.inherited_uniforms == 0, "live: a textured-snapshot class draw carries mask 0");
    // The sprite arm too (a different class, the same route).
    DrawIr t = Sprite(true); t.identity.hle_handler = 7; t.tex.tbp = 0x380000;
    w.pipe.Draw(t, "vs_sprite_text");
    CHECK(w.pipe.LastSource().owner.kind == TextureSource::OwnerKind::Decoded &&
              w.be.last_params.inherited_uniforms == 0,
          "live: textured sprite-HUD draw carries mask 0");
    // Untextured class draw: no decode route, the arm stays.
    DrawIr u = Sprite(false); u.identity.hle_handler = 7;
    w.pipe.Draw(u, "vs_sprite_text");
    CHECK(w.pipe.LastSource().owner.kind == TextureSource::OwnerKind::None &&
              w.be.last_params.inherited_uniforms == ClassRenderer::kInheritSpriteHudArm,
          "live: untextured sprite keeps the sprite arm");
    // A live target serve (the reference's RT path runs the generic program): the
    // arm stays -- the fullscreen blit shape reading a CT32 target it drew.
    DrawIr into = Raw(); into.raster.frame.fbp = 0x100000;
    w.pipe.Draw(into, nullptr);
    DrawIr rt = Blit(); rt.tex.tbp = 0x100000;
    w.pipe.Draw(rt, nullptr);
    CHECK(w.pipe.LastSource().owner.kind == TextureSource::OwnerKind::Surface, "live: target serve");
    CHECK(w.be.last_params.inherited_uniforms == ClassRenderer::kInheritFmvBlitArm, "live: a target serve keeps the blit arm");
    // Back to replay: the same textured draw declines again.
    w.pipe.SetLiveSnapshot(false);
    w.pipe.Draw(k, "vs_skinned_lit_strip");
    CHECK(w.pipe.LastSource().declined == TextureSource::Declined::NoSnapshot &&
              w.be.last_params.inherited_uniforms == ClassRenderer::kInheritSkinnedArm,
          "replay again: declined, arm rides");
}

// ---- sequence recipe (OLD test_s9_composite) ---------------------------------
static void TestSequence() {
    const SequenceRecipe::Constants& K = Profile().Sequence();
    SequenceRecipe q(K);
    CHECK(K.clear_rgba == 0x78787878u && K.stencil_base == 0x0F && K.counter_mask == 0x1F && K.quant_bit == 0x10, "constants");
    CHECK(K.stencil_base == (K.clear_rgba & 0xFF) >> 3, "base == clear byte >> 3");
    for (int net = -40; net <= 40; net++) {
        const uint8_t st = q.StencilAfter(net);
        CHECK(q.MaterializeByte(st) == q.OracleWrapByte(net), "byte == 8*stencil mod 256 for every net");
    }
    CHECK(q.MaterializeByte(0x0F) == 0x78 && q.MaterializeByte(0x10) == 0x80 && q.MaterializeByte(0xFF) == 0xF8, "materialize bytes");
    CHECK(q.OracleWrapByte(17) == 0x00 && q.OracleWrapByte(-15) == 0x00 && q.OracleWrapByte(16) == 0xF8, "wrap");
    CHECK(q.ClassifyStripColor(0x08, 0x08, 0x08) == SequenceRecipe::kStripIncr, "incr colour");
    CHECK(q.ClassifyStripColor(0xF8, 0xF8, 0xF8) == SequenceRecipe::kStripDecr, "decr colour");
    CHECK(q.ClassifyStripColor(0x08, 0x08, 0x09) == SequenceRecipe::kStripDecline, "other colour");
    CHECK(SequenceRecipe::Csm1Swizzle(8) == 16 && SequenceRecipe::Csm1Swizzle(16) == 8 && SequenceRecipe::Csm1Swizzle(3) == 3 &&
              SequenceRecipe::Csm1Swizzle(40) == 48, "CSM1 grid");
    uint8_t out[4];
    SequenceRecipe::InvertFold(10, 20, 30, 16, out);
    CHECK(out[0] == 245 && out[1] == 235 && out[2] == 225 && out[3] == 255 - 16, "invert fold idx 8 -> CSM1 16");
    SequenceRecipe::InvertFold(0, 0, 0, 255, out);
    CHECK(out[3] == 255 - 128, "alpha 255 -> idx 128");
    // Strips + clear predicates.
    DrawIr s = Strip();
    for (VertexRec& v : s.verts) { v.r = v.g = v.b = 0x08; }
    CHECK(q.ClassifyStripVerts(s) == SequenceRecipe::kStripIncr, "all-vertex incr");
    s.verts[1].g = 0x09;
    CHECK(q.ClassifyStripVerts(s) == SequenceRecipe::kStripDecline, "mixed colours decline");
    DrawIr c = Raw(); c.verts.clear(); AddQuad(c, 0, 0, 256, 224, 0x78787878u);
    CHECK(q.ClearPrimes(c), "0x78787878 primes");
    c.verts[3].a = 0x79;
    CHECK(!q.ClearPrimes(c), "other clear colour does not prime");
    DrawIr inv = Raw(); inv.verts.clear(); AddQuad(inv, 0, 0, 512, 448, 0x00808080u);
    CHECK(q.InvertVertexColorOk(inv), "invert vertex colour");
    inv.verts[0].a = 0x80;
    CHECK(!q.InvertVertexColorOk(inv), "invert alpha must be 0");
    // FSM.
    CHECK(q.GetState() == SequenceRecipe::kIdle && !q.Live(), "idle");
    q.Prime(0x230000);
    CHECK(q.GetState() == SequenceRecipe::kPrimed && q.LiveOn(0x230000) && !q.LiveOn(0x70000), "primed");
    DrawIr seed = Raw(); seed.raster.frame.fbp = 0x230000; seed.raster.frame.fbmsk = 0x00FFFFFFu;
    CHECK(q.OnPreDraw(seed, false) == SequenceRecipe::Action::kNone && q.GetState() == SequenceRecipe::kPrimed, "RGB-protected SEED keeps PRIMED");
    DrawIr other = Raw(); other.raster.frame.fbp = 0x70000;
    CHECK(q.OnPreDraw(other, false) == SequenceRecipe::Action::kNone, "another buffer is inert while PRIMED");
    DrawIr rgb = Raw(); rgb.raster.frame.fbp = 0x230000; rgb.raster.frame.fbmsk = 0xff000000u;
    CHECK(q.OnPreDraw(rgb, false) == SequenceRecipe::Action::kDemote && q.GetState() == SequenceRecipe::kIdle, "RGB write demotes");
    CHECK(q.Count().demotes == 1 && q.Count().primes == 1, "demote counted");
    q.Prime(0x230000); q.Arm(false);
    CHECK(q.GetState() == SequenceRecipe::kArmed && !q.ExtentValid() && q.HoldsEntry(0x230000) && !q.HoldsEntry(0x70000), "armed, hold");
    q.ExtendExtent(10, 20, 30, 40); q.ExtendExtent(5, 25, 20, 60); q.ExtendExtent(0, 0, 0, 0);
    CHECK(q.ExtentValid() && q.ExtX0() == 5 && q.ExtY0() == 20 && q.ExtX1() == 30 && q.ExtY1() == 60, "extent union");
    CHECK(q.OnPreDraw(other, false) == SequenceRecipe::Action::kNone && q.GetState() == SequenceRecipe::kArmed, "another buffer is inert while ARMED");
    DrawIr reads = Raw(); reads.raster.frame.fbp = 0x70000; reads.tex_enabled = true; reads.tex.tbp = 0x230000;
    CHECK(q.OnPreDraw(reads, false) == SequenceRecipe::Action::kMaterialize && q.GetState() == SequenceRecipe::kIdle, "texture read materialises");
    CHECK(q.Fbp() == 0x230000 && q.ExtentValid(), "fbp / extent survive for the materialise");
    q.Prime(0x230000); q.Arm(true);
    CHECK(q.OnPreDraw(rgb, false) == SequenceRecipe::Action::kMaterialize, "draw into the fbp materialises");
    q.Prime(0x230000); q.Arm(false);
    CHECK(q.OnPreDraw(other, true) == SequenceRecipe::Action::kMaterialize, "leaving a capture materialises");
    q.Prime(0x230000); q.Arm(false);
    CHECK(q.OnTouch(0x70000) == SequenceRecipe::Action::kNone && q.OnTouch(0x230000) == SequenceRecipe::Action::kMaterialize, "touch of the fbp");
    q.Prime(0x230000);
    CHECK(q.OnTouch(0x230000) == SequenceRecipe::Action::kNone && q.GetState() == SequenceRecipe::kPrimed, "touch while PRIMED is inert");
    CHECK(q.Count().arms == 4 && q.Count().materializes == 4, "counters");
    // Composite Accept fields (clear / invert) as specs.
    const title::RendererProfile::Shapes& S = Profile().Shape();
    DrawIr clr = Raw(); clr.verts.clear(); AddQuad(clr, 0, 0, 256, 224, 0x78787878u);
    clr.prim_type = 6; clr.use_fst = true; clr.raster.colclamp = true; clr.raster.test.zte = true; clr.raster.test.ztst = 1;
    clr.raster.zbuf.zmsk = true; clr.raster.frame.psm = 0x0A; clr.raster.frame.fbp = 0x230000;
    CHECK(S.composite_clear.Matches(clr), "clear fields");
    { DrawIr n = clr; n.raster.frame.fbmsk = 1; CHECK(!S.composite_clear.Matches(n), "clear: fbmsk"); }
    { DrawIr n = clr; n.raster.frame.psm = 0; CHECK(!S.composite_clear.Matches(n), "clear: CT16S"); }
    { DrawIr n = clr; n.tex_enabled = true; CHECK(!S.composite_clear.Matches(n), "clear: untextured"); }
    DrawIr d = Raw(); d.verts.clear(); AddQuad(d, 0, 0, 512, 448, 0x00808080u);
    d.prim_type = 6; d.use_fst = true; d.tex_enabled = true; d.tex.psm = 0x1B; d.tex.tfx = 3; d.tex.tcc = 1;
    d.tex.tbp = d.raster.frame.fbp; d.tex.tbw = 512; d.alpha_blend = true; d.raster.alpha = DrawIr::Alpha{0, 1, 2, 2, 0x80};
    d.raster.colclamp = true; d.raster.test.zte = true; d.raster.test.ztst = 1; d.raster.zbuf.zmsk = true;
    CHECK(S.composite_invert.Matches(d), "invert fields");
    { DrawIr n = d; n.raster.alpha.fix = 0x7F; CHECK(!S.composite_invert.Matches(n), "invert: FIX"); }
    { DrawIr n = d; n.tex.tfx = 0; CHECK(!S.composite_invert.Matches(n), "invert: HIGHLIGHT2"); }
    { DrawIr n = d; n.tex.cpsm = 0x02; CHECK(!S.composite_invert.Matches(n), "invert: CLUT CT32"); }
    // OLD test_s9_composite §5: the identity-invert CLUT walk over a synthetic linear grid.
    {
        auto addr_of = [](int x, int y) -> uint32_t { return static_cast<uint32_t>((y * 16 + x) * 4); };
        uint8_t mem[16 * 16 * 4];
        std::memset(mem, 0, sizeof(mem));
        for (uint32_t i = 0; i < 256; i++) {
            const uint32_t pp = SequenceRecipe::Csm1Swizzle(i);
            const uint32_t a = addr_of(static_cast<int>(pp % 16), static_cast<int>(pp / 16));
            mem[a] = mem[a + 1] = mem[a + 2] = 0xFF; mem[a + 3] = static_cast<uint8_t>(255u - i);
        }
        auto byte_at = [&](uint32_t a) -> uint8_t { return mem[a % sizeof(mem)]; };
        CHECK(SequenceRecipe::ClutIsInvertTable(byte_at, addr_of), "live CSM1 table accepted");
        CHECK(byte_at(addr_of(8, 0) + 3) == 0xEF, "physical position 8 holds A = 255 - CSM1(8)");
        mem[addr_of(7, 0) + 3] ^= 1;
        CHECK(!SequenceRecipe::ClutIsInvertTable(byte_at, addr_of), "one flipped alpha byte rejects");
        mem[addr_of(7, 0) + 3] ^= 1;
        CHECK(SequenceRecipe::ClutIsInvertTable(byte_at, addr_of), "restored accepts again");
        for (uint32_t pp = 0; pp < 256; pp++) mem[addr_of(static_cast<int>(pp % 16), static_cast<int>(pp / 16)) + 3] = static_cast<uint8_t>(255u - pp);
        CHECK(!SequenceRecipe::ClutIsInvertTable(byte_at, addr_of), "pure identity layout (no CSM1) rejects");
        std::memset(mem, 0, sizeof(mem));
        CHECK(!SequenceRecipe::ClutIsInvertTable(byte_at, addr_of), "zero mirror (replay) rejects");
        bool invol = true;
        for (uint32_t i = 0; i < 256; i++) invol = invol && SequenceRecipe::Csm1Swizzle(SequenceRecipe::Csm1Swizzle(i)) == i;
        CHECK(invol && SequenceRecipe::Csm1Swizzle(8) == 16 && SequenceRecipe::Csm1Swizzle(16) == 8 &&
              SequenceRecipe::Csm1Swizzle(3) == 3 && SequenceRecipe::Csm1Swizzle(24) == 24, "CSM1 involution");
    }
    // OLD §6: the folded invert == the generic-chain reference over the alpha sweep.
    {
        bool same = true;
        for (int cda = 0; cda < 256; cda++)
            for (int rgb : {0x00, 0x01, 0x40, 0x78, 0x7F, 0x80, 0xDB, 0xFE, 0xFF}) {
                uint8_t a[4], b4[4];
                SequenceRecipe::InvertFold(uint8_t(rgb), uint8_t(255 - rgb), uint8_t(cda), uint8_t(cda), a);
                SequenceRecipe::InvertGenericRef(uint8_t(rgb), uint8_t(255 - rgb), uint8_t(cda), uint8_t(cda), b4);
                same = same && std::memcmp(a, b4, 4) == 0;
            }
        CHECK(same, "folded invert == generic reference for every alpha x 9 rgb");
        uint8_t out[4];
        SequenceRecipe::InvertFold(0x87, 0xDB, 0xFD, 0x80, out);
        CHECK(out[0] == 0x78 && out[1] == 0x24 && out[2] == 0x02 && out[3] == 0xBF, "Z=0x40FDDB87 -> ~Z pin");
        SequenceRecipe::InvertFold(0, 0, 0, 0x10, out); CHECK(out[3] == 0xEF, "idx 8 -> A 0xEF");
        SequenceRecipe::InvertFold(0, 0, 0, 0x20, out); CHECK(out[3] == 0xF7, "idx 16 -> A 0xF7");
    }
}

// ---- the sequence hooks through the pipeline (6E review F2/F3 + ZSRC) ----------
static void TestHooks() {
    World w;
    World::RecordingMaterializer mat;
    w.pipe.SetMaterializer(&mat);
    DrawIr acc = Strip();                       // a draw into the accumulator 0x230000
    DrawIr other = Raw();                       // 0x70000
    w.pipe.Draw(acc, nullptr);                  // bind the accumulator (active target)
    CHECK(w.pipe.ActiveTargetFbp() == 0x230000, "active target = last bound");
    // (8) leaving capture: ARMED on the bound target, a draw into ANOTHER buffer materialises.
    w.sequence.Prime(0x230000); w.sequence.Arm(false);
    w.pipe.Draw(other, nullptr);
    CHECK(mat.calls == 1 && w.sequence.GetState() == SequenceRecipe::kIdle, "leaving the bound accumulator materialises");
    CHECK(w.pipe.ActiveTargetFbp() == 0x70000, "active target moved");
    // Not leaving: ARMED but the accumulator is NOT the bound target -> inert.
    w.sequence.Prime(0x230000); w.sequence.Arm(false);
    w.pipe.Draw(other, nullptr);
    CHECK(mat.calls == 1 && w.sequence.GetState() == SequenceRecipe::kArmed, "another buffer while unbound is inert");
    // (9) hook before the guards: a 2-vertex draw into the armed fbp materialises (draws nothing).
    DrawIr two = acc; two.verts.pop_back();
    const int draws_before = w.be.draws;
    w.pipe.Draw(two, nullptr);
    CHECK(mat.calls == 2 && w.sequence.GetState() == SequenceRecipe::kIdle && w.be.draws == draws_before, "2-vertex draw into the fbp: hook fires, guard rejects");
    w.sequence.Prime(0x230000); w.sequence.Arm(false);
    DrawIr fbw0 = acc; fbw0.raster.frame.fbw = 0;
    w.pipe.Draw(fbw0, nullptr);
    CHECK(mat.calls == 3 && w.be.draws == draws_before, "fbw 0 draw into the fbp: hook fires, guard rejects");
    // A 1-vertex draw is below the reference's first guard: no hook.
    w.sequence.Prime(0x230000); w.sequence.Arm(false);
    DrawIr one = acc; one.verts.resize(1);
    w.pipe.Draw(one, nullptr);
    CHECK(mat.calls == 3 && w.sequence.GetState() == SequenceRecipe::kArmed, "1-vertex draw: below the hook");
    // Touch hooks: a textured resolve of the armed fbp (resolver entry) materialises.
    DrawIr reads = Raw(); reads.tex_enabled = true; reads.tex.tbp = 0x230000; reads.tex.psm = 0x0A; reads.tex.tw = 8; reads.tex.th = 8;
    w.pipe.Draw(reads, nullptr);
    CHECK(mat.calls == 4 && w.sequence.GetState() == SequenceRecipe::kIdle, "texture read of the armed fbp materialises (resolver touch)");
    // A writeback of the armed target (SyncAll -> SyncDrawn) materialises through the cache hook.
    w.sequence.Prime(0x230000); w.sequence.Arm(false);
    w.pipe.OnSyncAll();
    CHECK(mat.calls == 5 && w.sequence.GetState() == SequenceRecipe::kIdle, "writeback of the armed fbp materialises (cache touch)");
    // The hold: the armed accumulator's own ledger entry is kept out of a held flush.
    w.pipe.Draw(acc, nullptr);                                  // 16-bit draw -> ledger entry for 0x230000
    CHECK(w.cache.Ledger().Has(0x230000), "16-bit draw recorded");
    w.sequence.Prime(0x230000); w.sequence.Arm(false);
    const uint32_t flushed = w.cache.FlushPending(PendingRtSyncLedger::Reason::PreDraw, 0x230000, 4, false, 0x230000);
    CHECK(flushed == 0 && w.cache.Ledger().Has(0x230000) && w.cache.HeldEntries() == 1 && mat.calls == 5, "held entry stays deferred, no touch");
    CHECK(w.cache.FlushPending(PendingRtSyncLedger::Reason::PreDraw, 0x230000, 4) == 1 && mat.calls == 6, "unheld flush deposits and touches");
    // No materializer installed: the action is counted as dropped, state still idles.
    w.pipe.SetMaterializer(nullptr);
    w.sequence.Prime(0x230000); w.sequence.Arm(false);
    w.pipe.Draw(acc, nullptr);
    CHECK(w.pipe.DroppedMaterializes() == 1 && w.sequence.GetState() == SequenceRecipe::kIdle, "dropped materialise counted");
    // ZSRC route: a Z-tested GEQUAL/GREATER, ZMSK, DATE draw whose ZBP names another live colour target.
    DrawIr zc = Raw(); zc.raster.frame.fbp = 0x268000; zc.raster.frame.psm = 0x30;   // the colour-authored Z copy
    w.pipe.Draw(zc, nullptr);
    DrawIr st = Strip(); st.raster.zbuf.zbp = 0x268000;
    w.pipe.Draw(st, nullptr);
    CHECK(w.be.last_params.zsrc_armed && w.be.last_params.zsrc.id == w.cache.Find(0x268000)->views.color.id &&
          w.be.last_params.zsrc_scale == 1, "zsrc armed against the zbp target's colour");
    { DrawIr n = st; n.raster.zbuf.zbp = 0x300000; w.pipe.Draw(n, nullptr); CHECK(!w.be.last_params.zsrc_armed, "no target at zbp"); }
    { DrawIr n = st; n.raster.test.date = false; w.pipe.Draw(n, nullptr); CHECK(!w.be.last_params.zsrc_armed, "zsrc needs DATE"); }
    { DrawIr n = st; n.raster.test.ztst = 1; w.pipe.Draw(n, nullptr); CHECK(!w.be.last_params.zsrc_armed, "zsrc needs GEQUAL/GREATER"); }
    { DrawIr n = st; n.raster.zbuf.zmsk = false; w.pipe.Draw(n, nullptr); CHECK(!w.be.last_params.zsrc_armed, "zsrc needs ZMSK"); }
    { DrawIr n = st; n.raster.zbuf.zbp = n.raster.frame.fbp; w.pipe.Draw(n, nullptr); CHECK(!w.be.last_params.zsrc_armed, "zsrc needs zbp != fbp"); }
    CHECK(w.pipe.ZsrcArms() == 1, "one arm counted");
}

// ---- shuffle snap (OLD test_shuf_snap) ---------------------------------------
static DrawIr ShuffleStrip(float x0, float u0, float w, float v0, float v1) {
    DrawIr ir = Raw(); ir.verts.clear();
    ir.prim_type = 6; ir.use_fst = true; ir.tex.fst = true; ir.tex_enabled = true;
    ir.raster.frame.psm = 0x02; ir.tex.psm = 0x32;
    auto U = [](float f) { return static_cast<uint16_t>(f * 16.0f); };
    AddVert(ir, x0, 0, U(u0), U(v0)); AddVert(ir, x0 + w, 0, U(u0 + w), U(v0)); AddVert(ir, x0, 448, U(u0), U(v1));
    AddVert(ir, x0 + w, 0, U(u0 + w), U(v0)); AddVert(ir, x0 + w, 448, U(u0 + w), U(v1)); AddVert(ir, x0, 448, U(u0), U(v1));
    return ir;
}
static bool Detect(const DrawIr& ir) {
    return TextureShuffleSnap::DetectColumnShuffle(ir.verts.data(), ir.verts.size(), ir.prim_type, ir.tex.fst,
                                                   ir.raster.frame.psm, static_cast<uint8_t>(ir.tex.psm), ir.tex_enabled);
}
static void TestSnap() {
    using S = TextureShuffleSnap;
    CHECK(S::SnapToColumn8(8.5f) == 8.0f && S::SnapToColumn8(12.0f) == 16.0f && S::SnapToColumn8(20.0f) == 16.0f && S::SnapToColumn8(28.0f) == 32.0f, "column snap half-even");
    CHECK(S::SnapToColumn8(16.5f) == 16.0f && S::SnapToColumn8(15.6f) == 16.0f, "column snap rounds");
    CHECK(S::SnapRow(0.5f) == 0.0f && S::SnapRow(448.5f) == 448.0f && S::SnapRow(3.0f) == 3.0f && S::SnapRow(10.5f) == 10.0f, "row snap keeps sub-8 offsets, sheds the bias");
    CHECK(S::IsPsm16(0x02) && S::IsPsm16(0x0A) && S::IsPsm16(0x32) && S::IsPsm16(0x3A) && !S::IsPsm16(0x00) && !S::IsPsm16(0x30), "16 bpp psms");
    // The live strip: x=[8k+8, 8k+16), u=[8k+0.5, 8k+8.5), v=[0.5, 448.5).
    DrawIr live = ShuffleStrip(8, 0.5f, 8, 0.5f, 448.5f);
    CHECK(Detect(live), "live strip detected");
    { DrawIr n = live; n.raster.frame.psm = 0x00; CHECK(!Detect(n), "32-bit frame"); }
    { DrawIr n = live; n.tex.psm = 0x00; CHECK(!Detect(n), "32-bit texture read"); }
    { DrawIr n = live; n.tex.fst = false; CHECK(!Detect(n), "STQ"); }
    { DrawIr n = live; n.tex_enabled = false; CHECK(!Detect(n), "untextured"); }
    { DrawIr n = live; n.prim_type = 4; CHECK(!Detect(n), "not a sprite"); }
    { DrawIr n = live; n.verts.pop_back(); CHECK(!Detect(n), "count % 6"); }
    CHECK(Detect(ShuffleStrip(16, 16.5f, 8, 0.5f, 448.5f)), "offset 0 mod 16");
    CHECK(!Detect(ShuffleStrip(12, 0.5f, 8, 0.5f, 448.5f)), "offset 12 declines");
    CHECK(!Detect(ShuffleStrip(8, 0.5f, 12, 0.5f, 448.5f)), "width 12 declines");
    CHECK(!Detect(ShuffleStrip(8, 0.5f, 4, 0.5f, 448.5f)), "width 4 declines (< 8)");
    CHECK(!Detect(ShuffleStrip(8, 3.0f, 8, 0.5f, 448.5f)), "u corner off the grid");
    CHECK(Detect(ShuffleStrip(8, 0.5f, 8, 2.5f, 100.5f)), "whole-row v offsets snap");
    CHECK(Detect(ShuffleStrip(8, 0.5f, 8, 2.9f, 100.5f)), "any whole-row offset snaps (SnapRow absorbs the fraction)");
    {   // x-width != u-width.
        DrawIr n = live; n.verts[1].uv_u = static_cast<uint16_t>(12.5f * 16); n.verts[3].uv_u = n.verts[1].uv_u; n.verts[4].uv_u = n.verts[1].uv_u;
        CHECK(!Detect(n), "u width != x width");
    }
    // OLD test_shuf_snap S3(c): a 16bpp read whose U extent is doubled but every U corner
    // stays on the 8-grid (x=[8,16) u=[0.5,16.5)) -- only the `xw == uw` predicate rejects it.
    {
        DrawIr n = live;
        const uint16_t u1 = static_cast<uint16_t>(16.5f * 16);
        n.verts[1].uv_u = u1; n.verts[3].uv_u = u1; n.verts[4].uv_u = u1;
        CHECK(!Detect(n), "S3(c): doubled U width on-grid declines (xw == uw is the sole rejecter)");
        DrawIr half = ShuffleStrip(8, 0.5f, 16, 0.5f, 448.5f);   // x width 16, u width 16: on-grid, widths equal
        CHECK(Detect(half), "S3(c) control: equal 16-wide x/u extents on-grid detect");
    }
    // Two quads, all-or-nothing.
    DrawIr two = live; for (const VertexRec v : ShuffleStrip(16, 8.5f, 8, 0.5f, 448.5f).verts) two.verts.push_back(v);
    CHECK(Detect(two), "two conforming quads");
    two.verts[6].uv_u = static_cast<uint16_t>(20.0f * 16);   // quad 2: u1 becomes 20, u width 11.5 != x width 8
    CHECK(!Detect(two), "one bad quad fails the batch");
    // Through the pipeline: the snap decision reaches the TextureSource.
    World w;
    w.pipe.Draw(live, nullptr);
    CHECK(w.pipe.LastSource().transform.shuffle_snap, "snap flagged on the source");
    w.pipe.Draw(Raw(), nullptr);
    CHECK(!w.pipe.LastSource().transform.shuffle_snap, "plain draw unsnapped");
}

// ---- pipeline order + the same-buffer flush -------------------------------------
static void TestPipeline() {
    World w;
    w.pipe.OnSetFrontBuffer(0x70000, 512, 0);
    w.pipe.OnSetWindow(640, 448, 512, 448);
    CHECK(w.scanout.ScanoutFbp() == 0x70000 && w.scanout.ContentW() == 512 && w.pipe.WindowW() == 640, "jobs applied");
    w.pipe.SetGfxEpoch(5);
    DrawIr d = Raw();
    w.pipe.Draw(d, nullptr);
    RenderTarget* t = w.cache.Find(0x70000);
    CHECK(t != nullptr, "step 2: target acquired");
    CHECK(w.be.draws == 1 && w.be.last_params.color.id == t->views.color.id, "step 6: drawn into the target");
    CHECK(w.be.last_params.render_w == 512 && w.be.last_params.render_h == 448, "step 1: content dims for the scanout target");
    CHECK(w.be.last_params.do_frame_clear && w.be.last_params.clear_color, "step 4: first draw clears the scanout target");
    CHECK(w.cache.MostRecentDrawnFbp() == 0x70000 && w.pipe.DrawSeq() == 1, "step 7: NoteDrawn + seq");
    CHECK(t->coherence.last_draw_gfx_epoch == 5, "gfx epoch stamped");
    w.pipe.Draw(d, nullptr);
    CHECK(!w.be.last_params.do_frame_clear, "second draw of the EE frame does not clear");
    // Same-buffer pre-draw flush: a 16-bit draw into 0x80000 records a ledger
    // entry; a 32-bit draw into the SAME buffer flushes it first; a 16-bit draw or
    // another buffer's draw does not.
    DrawIr s16 = Raw(); s16.raster.frame.fbp = 0x80000; s16.raster.frame.psm = 0x02; s16.raster.frame.fbw = 64;
    w.pipe.Draw(s16, nullptr);
    CHECK(w.cache.Ledger().Has(0x80000), "16-bit draw recorded");
    DrawIr other32 = Raw(); other32.raster.frame.fbp = 0x100000;
    w.pipe.Draw(other32, nullptr);
    CHECK(w.cache.Ledger().Has(0x80000) && w.pipe.SameBufferFlushes() == 0, "another buffer's 32-bit draw leaves the entry");
    w.pipe.Draw(s16, nullptr);
    CHECK(w.cache.Ledger().Has(0x80000) && w.pipe.SameBufferFlushes() == 0, "a 16-bit draw leaves the entry");
    DrawIr same32 = Raw(); same32.raster.frame.fbp = 0x80000; same32.raster.frame.fbw = 64;
    w.pipe.Draw(same32, nullptr);
    CHECK(w.pipe.SameBufferFlushes() == 1 && w.cache.Ledger().Flushes(PendingRtSyncLedger::Reason::PreDraw) >= 1,
          "same-buffer 32-bit draw flushed the deferred entry first");
    // Composite consumption: a draw into the scanout target reading another target.
    DrawIr comp = Raw(); comp.tex_enabled = true; comp.tex.tbp = 0x100000; comp.tex.psm = 0; comp.tex.tw = 6; comp.tex.th = 6; comp.tex.tbw = 64;
    RenderTarget* src = w.cache.Find(0x100000);
    CHECK(src && !src->coherence.ever_composited, "source not yet consumed");
    w.pipe.Draw(comp, nullptr);
    CHECK(src->coherence.ever_composited && src->coherence.needs_frame_clear, "source marked composite-consumed");
    // Frame end: mark, present flush, grab, quad.
    DrawPipeline::Frame f;
    w.pipe.OnFrameEnd(true, CrtcClassifier::Snapshot{}, &f);
    CHECK(w.presenter.EeFrameCount() == 1 && w.presenter.FrameCount() == 1, "frame counters");
    CHECK(f.grabbed && f.w == 512 && f.h == 448 && f.rgba.size() == 512u * 448 * 4, "grab of the scanout target");
    CHECK(w.be.presents == 1 && w.be.last_present.source.id == t->views.color.id, "display quad from the target");
    w.pipe.Draw(d, nullptr);
    CHECK(w.be.last_params.do_frame_clear, "new EE frame clears again");
    // The corpus replay's frame end: the same mark / present flush / grab, then the
    // swap alone -- no display quad, no scanout-side deposit or ledger record (the
    // reference replay's present never ran the scanout composite).
    {
        const uint64_t deposits_before = w.vram.Page(0x70000 / 8192).shadow_epoch;
        const size_t ledger_before = w.cache.Ledger().Count();
        const int presents_before = w.be.presents;
        DrawPipeline::Frame g;
        w.pipe.OnFrameEndGrabOnly(true, &g);
        CHECK(w.presenter.EeFrameCount() == 2 && w.presenter.FrameCount() == 2, "grab-only frame end advances both counters");
        CHECK(g.grabbed && g.w == 512 && g.h == 448, "grab-only frame end still grabs the scanout target");
        CHECK(w.be.presents == presents_before + 1 && w.be.last_present.source.id == 0, "swap with no display quad");
        CHECK(w.presenter.LastPresent().source == ScanoutComposer::Source::None, "no pick recorded");
        CHECK(w.vram.Page(0x70000 / 8192).shadow_epoch == deposits_before && w.cache.Ledger().Count() == ledger_before,
              "no scanout-side mirror maintenance");
        w.pipe.Draw(d, nullptr);
        CHECK(w.be.last_params.do_frame_clear, "the EE frame mark of the grab-only end clears again");
    }
    // Guards: fewer than three vertices / no FBW draw nothing.
    const int before = w.be.draws;
    DrawIr tiny = Raw(); tiny.verts.pop_back();
    w.pipe.Draw(tiny, nullptr);
    DrawIr nofbw = Raw(); nofbw.raster.frame.fbw = 0;
    w.pipe.Draw(nofbw, nullptr);
    CHECK(w.be.draws == before, "entry guards");
    // Invalidate / upload / sync-all jobs reach their owners.
    w.pipe.OnInvalidate(0x80000, 0x2000);
    CHECK(w.refresher.ArePagesCpuDirty(0x80000, 0x2000), "invalidate marks CPU-dirty");
    std::vector<uint8_t> px(64 * 4 * 4, 0x11);
    w.pipe.OnUpload(0x300000, 64, 4, px.data(), 64 * 4 * 4);
    CHECK(w.refresher.UploadedCount() == 1, "upload recorded");
    w.pipe.Draw(s16, nullptr);
    w.pipe.OnSyncAll();
    CHECK(w.cache.Ledger().Empty(), "sync-all empties the ledger");
}

// ---- counter grammar (INV 2.9 lines) -------------------------------------------
static void TestGrammar() {
    World w;
    w.profile.RegisterClassRenderers(w.registry);
    DrawIr k = Skinned();
    DrawIr hle = k; hle.identity.hle_handler = 3;
    for (int i = 0; i < 5; i++) w.pipe.Draw(hle, "vs_skinned_lit_strip");
    for (int i = 0; i < 13; i++) w.pipe.Draw(k, nullptr);
    DrawIr raw = Raw();
    for (int i = 0; i < 4; i++) w.pipe.Draw(raw, nullptr);
    DrawIr disp = Raw(); disp.identity.hle_handler = 9;
    w.pipe.Draw(disp, "vs_indexed_displace");
    DrawIr world2 = k; world2.identity.hle_handler = 4;
    w.pipe.Draw(world2, "vs_family_tristrip");
    DrawIr t = Sprite(true), u = Sprite(false);
    w.pipe.Draw(t, nullptr); w.pipe.Draw(u, nullptr);
    char buf[4096];
    std::FILE* f = fmemopen(buf, sizeof(buf), "w");
    w.registry.EmitCounters(f);
    w.profile.EmitRendererCounters(f);
    std::fclose(f);
    const std::string out(buf);
    const char* want =
        "[GS-CLASSRDR] class=world-tristrip seen=1 verts=3 native=1 fb_norenderer=0 fb_disabled=0 declined=0\n"
        "[GS-CLASSRDR] class=skinned seen=5 verts=15 native=5 fb_norenderer=0 fb_disabled=0 declined=0\n"
        "[GS-CLASSRDR] class=indexed-displace seen=1 verts=3 native=0 fb_norenderer=1 fb_disabled=0 declined=0\n"
        "[GS-CLASSRDR] class=raw-sprites-HUD seen=2 verts=12 native=0 fb_norenderer=2 fb_disabled=0 declined=0\n"
        "[GS-CLASSRDR] class=raw-other seen=17 verts=51 native=13 fb_norenderer=0 fb_disabled=0 declined=4\n"
        "[GS-CLASSRDR] SUMMARY on=1 renderers=6 seen=26 native=19 fallback=7 declined=4\n"
        "[GS-SKINRDR] native=19 spec=0 generic=19 skin=5 world2=1 raw=13 rt_serve=0\n";
    CHECK(out == want, "counter grammar");
    if (out != want) std::printf("--- got:\n%s--- want:\n%s", out.c_str(), want);
}

// ---- profile registration ------------------------------------------------------
static void TestProfile() {
    World w;
    CHECK(w.profile.RegisterClassRenderers(w.registry) == 6 && w.registry.RegisteredCount() == 6, "6 slots without the sequence FSM");
    CHECK(!std::strcmp(w.registry.Slot(C::kWorldTristrip)->Name(), "world-tristrip"), "world name");
    CHECK(!std::strcmp(w.registry.Slot(C::kSkinned)->Name(), "skinned"), "skinned name");
    CHECK(!std::strcmp(w.registry.Slot(C::kParticlesUntex)->Name(), "particles-untex"), "particles name");
    CHECK(!std::strcmp(w.registry.Slot(C::kSpritesHud)->Name(), "sprites-HUD"), "sprites name");
    CHECK(!std::strcmp(w.registry.Slot(C::kRawBlitFullscreen)->Name(), "raw-blit-fullscreen"), "blit name");
    CHECK(!std::strcmp(w.registry.Slot(C::kRawOther)->Name(), "raw-skin"), "raw-other = raw skin");
    CHECK(w.registry.Slot(C::kIndexedDisplace) == nullptr && w.registry.Slot(C::kHleOther) == nullptr &&
              w.registry.Slot(C::kRawSpritesHud) == nullptr, "no renderer for displace / hle-other / raw HUD");
    CHECK(w.registry.Slot(C::kStencilComposite) == nullptr, "stencil composite: unregistered without the sequence (fb_norenderer)");
    CHECK(w.registry.WarmUpAll(w.pipe, nullptr) == 0, "replica renderers warm no programs");
    CHECK(Profile().ClassRules().stamps.size() == 7, "seven stamps");
    // The peak registration: the stencil composite over the pipeline's sequence FSM.
    World w7;
    CHECK(w7.profile.RegisterClassRenderers(w7.registry, &w7.sequence) == 7 && w7.registry.RegisteredCount() == 7, "7 slots with the sequence");
    CHECK(w7.registry.Slot(C::kStencilComposite) && !std::strcmp(w7.registry.Slot(C::kStencilComposite)->Name(), "S9-shadow"), "S9-shadow name");
    CHECK(w7.registry.WarmUpAll(w7.pipe, nullptr) == 0, "the S9 programs are the backend's (warm 0)");
}

// ---- the stencil-composite renderer (6E'': OLD s9_composite_native.inc) -----------
static DrawIr ClearIr() {
    DrawIr ir = Raw(); ir.verts.clear(); AddQuad(ir, 0, 0, 128, 128, 0x78787878u);
    ir.prim_type = 6; ir.use_fst = true; ir.raster.colclamp = true; ir.raster.test.zte = true; ir.raster.test.ztst = 1;
    ir.raster.zbuf.zmsk = true; ir.raster.frame.psm = 0x0A; ir.raster.frame.fbmsk = 0; ir.raster.frame.fbp = 0x230000;
    ir.raster.frame.fbw = 128; ir.raster.scissor = DrawIr::Scissor{0, 0, 127, 127};
    return ir;
}
static DrawIr S9Strip(uint8_t rgb, float x0, float y0, float x1, float y1) {
    DrawIr ir = Strip(); ir.verts.clear();
    const uint32_t c = 0x80000000u | (uint32_t(rgb) << 16) | (uint32_t(rgb) << 8) | rgb;
    AddVert(ir, x0, y0, 0, 0, c); AddVert(ir, x1, y0, 0, 0, c); AddVert(ir, x0, y1, 0, 0, c);
    ir.raster.zbuf.zbp = 0x268000; ir.raster.frame.fbw = 128; ir.raster.scissor = DrawIr::Scissor{0, 0, 127, 127};
    return ir;
}
static DrawIr InvertIr(uint32_t cbp) {
    DrawIr ir = Raw(); ir.verts.clear(); AddQuad(ir, 0, 0, 64, 64, 0x00808080u);
    ir.prim_type = 6; ir.use_fst = true; ir.tex.fst = true; ir.alpha_blend = true;
    ir.raster.alpha = DrawIr::Alpha{0, 1, 2, 2, 0x80}; ir.raster.colclamp = true; ir.raster.test.zte = true;
    ir.raster.test.ztst = 1; ir.raster.zbuf.zmsk = true; ir.raster.frame.fbmsk = 0;
    ir.tex_enabled = true; ir.tex.psm = 0x1B; ir.tex.tfx = 3; ir.tex.tcc = 1; ir.tex.cpsm = 0; ir.tex.csm = 0; ir.tex.csa = 0;
    ir.tex.tbp = ir.raster.frame.fbp; ir.tex.tbw = ir.raster.frame.fbw; ir.tex.wms = 1; ir.tex.wmt = 1; ir.tex.tw = 9; ir.tex.th = 9;
    ir.tex.cbp = cbp;
    return ir;
}
static void WriteInvertClut(VramImage& vram, uint32_t cbp) {
    uint8_t* v = vram.MutableBytes();
    for (uint32_t p = 0; p < 256; p++) {
        const uint32_t a = PsmLayout::PixelByteAddress(Psm::CT32, p % 16, p / 16, cbp, 64) % VramImage::kBytes;
        v[a] = v[a + 1] = v[a + 2] = 0xFF; v[a + 3] = uint8_t(255u - SequenceRecipe::Csm1Swizzle(p));
    }
}
using SC = test::FakeBackend::StencilCall;
using Counters = StencilCompositeRenderer::Counters;
static bool StEq(const render::StencilState& a, render::StencilState::Func f, uint8_t ref, uint8_t cmp, uint8_t wr,
                 render::StencilState::Op op) {
    return a.enabled && a.func == f && a.ref == ref && a.cmp_mask == cmp && a.write_mask == wr && a.pass_op == op;
}
static void TestS9Passes() {
    World w;
    CHECK(w.profile.RegisterClassRenderers(w.registry, &w.sequence) == 7, "seven slots with the sequence");
    auto* s9 = dynamic_cast<StencilCompositeRenderer*>(w.registry.Slot(C::kStencilComposite));
    CHECK(s9 && !std::strcmp(s9->Name(), "S9-shadow"), "S9 slot registered");
    w.registry.WarmUpAll(w.pipe, nullptr);
    DrawIr zc = Raw(); zc.raster.frame.fbp = 0x268000; zc.raster.frame.psm = 0x30; w.pipe.Draw(zc, nullptr);
    // CLEAR: generic draw + PRIMED.
    DrawIr clr = ClearIr();
    CHECK(s9->Accept(clr) && w.pipe.Draw(clr, nullptr), "CLEAR native");
    CHECK(w.sequence.GetState() == SequenceRecipe::kPrimed && w.sequence.Fbp() == 0x230000, "CLEAR primes");
    CHECK(s9->Count().clears == 1 && s9->Count().primes == 1, "clears / primes counted");
    CHECK(w.be.stencil_calls.empty(), "no stencil work on the CLEAR");
    // First strip: ARM (stencil cleared to base) + the three passes; the ledger record.
    const int draws0 = w.be.draws;
    DrawIr s1 = S9Strip(0x08, 0, 0, 64, 64);
    CHECK(w.pipe.Draw(s1, nullptr), "strip native (via the world slot)");
    CHECK(w.sequence.GetState() == SequenceRecipe::kArmed && w.sequence.Alt16() == false, "first strip arms");
    CHECK(s9->StencilSurface().id != 0 && w.be.DescribeSurface(s9->StencilSurface()).format == render::SurfaceFormat::DepthStencil24_8, "D24S8 stencil surface");
    const auto& sc = w.be.stencil_calls;
    CHECK(sc.size() == 4, "clear + pass 1 + two quads");
    if (sc.size() == 4) {
        CHECK(sc[0].kind == SC::kClear && sc[0].clear_value == 0x0F && sc[0].stencil.id == s9->StencilSurface().id, "ARM: ClearStencil(base 0x0F)");
        CHECK(sc[1].kind == SC::kDraw && sc[1].program == render::StencilProgram::StripCount &&
              StEq(sc[1].st, render::StencilState::kEqual, 0x00, 0x80, 0x80, render::StencilState::kInvert), "pass 1: StripCount {Equal 0 0x80 0x80 Invert}");
        CHECK(sc[2].kind == SC::kQuad && sc[2].color.id == 0 && StEq(sc[2].st, render::StencilState::kEqual, 0x80, 0x80, 0x1F, render::StencilState::kIncrWrap) &&
              sc[2].scissor.x == 0 && sc[2].scissor.y == 0 && sc[2].scissor.w == 64 && sc[2].scissor.h == 64, "pass 2: bbox quad INCR_WRAP counter");
        CHECK(sc[3].kind == SC::kQuad && StEq(sc[3].st, render::StencilState::kEqual, 0x80, 0x80, 0x80, render::StencilState::kInvert), "pass 3: bbox quad clears bit 7");
    }
    CHECK(w.be.draws == draws0 + 1 && w.be.last_params.program == render::StencilProgram::StripCount &&
          w.be.last_params.zsrc_armed && w.be.last_params.stencil.id == s9->StencilSurface().id, "one backend draw: the stencil pass with ZSRC");
    CHECK(w.cache.Ledger().Has(0x230000), "the per-strip CT16S ledger record");
    CHECK(w.cache.Find(0x230000)->coherence.sync_keep_mask16 == 0x8000, "the stencil strip records the A-protect keep mask like a colour strip");
    CHECK(s9->Count().arms == 1 && s9->Count().strips_stencil == 1 && s9->Count().dedupe_draws == 2, "arm / stencil strip / dedupe counted");
    // Second strip (-8): DECR_WRAP, the hold keeps the accumulator's entry, the extent grows.
    DrawIr s2 = S9Strip(0xF8, 32, 32, 96, 96);
    w.pipe.Draw(s2, nullptr);
    CHECK(sc.size() == 7 && sc[5].kind == SC::kQuad && sc[5].st.pass_op == render::StencilState::kDecrWrap &&
          sc[5].scissor.x == 32 && sc[5].scissor.w == 64, "-8 strip: DECR_WRAP over its bbox");
    CHECK(w.cache.HeldEntries() == 1 && w.cache.Ledger().Has(0x230000) && w.sequence.GetState() == SequenceRecipe::kArmed, "the ladder's own flush holds the armed entry");
    CHECK(w.sequence.ExtX0() == 0 && w.sequence.ExtY0() == 0 && w.sequence.ExtX1() == 96 && w.sequence.ExtY1() == 96, "extent = union of the strip rects");
    CHECK(s9->Count().strips_stencil == 2 && s9->Count().materializes == 0, "two stencil strips, nothing materialised");
    // A strip clipped by the scissor: the quad rect is the bbox within the scissor.
    DrawIr s3 = S9Strip(0x08, 100, 100, 200, 200); s3.raster.scissor = DrawIr::Scissor{0, 0, 119, 127};
    w.pipe.Draw(s3, nullptr);
    CHECK(sc.size() == 10 && sc[8].scissor.x == 100 && sc[8].scissor.w == 20 && sc[8].scissor.h == 28, "strip rect = bbox within the scissor, clamped to the stencil");
    // MATERIALIZE: an RGB draw into the accumulator -> 32 quads over the extent, stamps, then idle.
    const RenderTarget* acc = w.cache.Find(0x230000);
    const uint64_t author0 = acc->coherence.main_author_seq;
    DrawIr rgb = Raw(); rgb.raster.frame.fbp = 0x230000; rgb.raster.frame.psm = 0x0A; rgb.raster.frame.fbw = 128;
    rgb.raster.scissor = DrawIr::Scissor{0, 0, 127, 127};   // same dims: no regrow under the materialised bytes
    const size_t before = sc.size();
    w.pipe.Draw(rgb, nullptr);
    CHECK(w.sequence.GetState() == SequenceRecipe::kIdle && s9->Count().materializes == 1 && s9->Count().mat_draws == 32, "draw into the fbp materialises");
    acc = w.cache.Find(0x230000);   // the RGB draw may have regrown the target
    CHECK(sc.size() == before + 32, "32 materialise quads");
    bool quads_ok = sc.size() == before + 32;
    for (int v = 0; v < 32 && quads_ok; v++) {
        const SC& q = sc[before + size_t(v)];
        const uint32_t b = uint32_t(8 * v) & 0xFF;
        quads_ok = q.kind == SC::kQuad && q.color.id == acc->views.color.id && q.stencil.id == s9->StencilSurface().id &&
                   StEq(q.st, render::StencilState::kEqual, uint8_t(v), 0x1F, 0x00, render::StencilState::kKeep) &&
                   q.rgba == (0xFF000000u | (b << 16) | (b << 8) | b) && !q.write_alpha &&
                   q.scissor.x == 0 && q.scissor.y == 0 && q.scissor.w == 120 && q.scissor.h == 128;
    }
    CHECK(quads_ok, "materialise quads: EQUAL v mask 0x1F, byte 8v, alpha protected, scissored to the extent");
    CHECK(acc->coherence.main_author_seq > author0 && acc->coherence.has_content &&
          acc->coherence.drawn_since_sync.x1 >= 120 && acc->coherence.drawn_since_sync.y1 >= 128, "materialise stamps the target (author, content, drawn rect)");
    if (sc.size() > before && w.be.Has(sc[before].color)) {
        const render::SurfaceId cv = sc[before].color; const uint32_t sw = w.be.DescribeSurface(cv).width;
        const std::vector<uint8_t>& px = w.be.At(cv).rgba;
        CHECK(cv.id == acc->views.color.id && px[(10 * sw + 10) * 4] == 0xF8 && px[(10 * sw + 10) * 4 + 3] != 0xFF, "bytes landed on the routed view (FakeBackend: last quad), alpha untouched");
    } else {
        CHECK(false, "materialise quads landed on a live surface");
    }
    // Steer: PRIMED + a strip that is not +-8 -> colour path + demote (no stencil work, no hook).
    w.pipe.Draw(clr, nullptr);
    CHECK(w.sequence.GetState() == SequenceRecipe::kPrimed, "re-primed");
    const size_t before2 = sc.size(); const int draws2 = w.be.draws;
    DrawIr odd = S9Strip(0x10, 0, 0, 32, 32);
    CHECK(w.pipe.Draw(odd, nullptr) && w.be.draws == draws2 + 1 && sc.size() == before2 && w.be.last_params.program == render::StencilProgram::None, "odd strip: colour path, one generic draw");
    CHECK(w.sequence.GetState() == SequenceRecipe::kIdle && s9->Count().demotes == 1 && s9->Count().strips_color == 1, "odd strip demotes the primed sequence");
    // Idle sequence: every strip takes the colour path.
    w.pipe.Draw(s1, nullptr);
    CHECK(s9->Count().strips_color == 2 && sc.size() == before2 && s9->Count().strips_stencil == 3, "idle: colour path");
    // ARMED + a non-conforming strip -> materialise then colour path.
    w.pipe.Draw(clr, nullptr); w.pipe.Draw(s1, nullptr);
    CHECK(w.sequence.GetState() == SequenceRecipe::kArmed && s9->Count().arms == 2, "armed again");
    DrawIr nozs = S9Strip(0x08, 0, 0, 32, 32); nozs.raster.zbuf.zbp = 0x300000;
    w.pipe.Draw(nozs, nullptr);
    CHECK(w.sequence.GetState() == SequenceRecipe::kIdle && s9->Count().materializes == 2 && s9->Count().strips_color == 3, "no ZSRC target: materialise + colour path");
    // Scaled target declines the prime.
    // (the harness profile pins scale 1; the decline arm is covered by the recipe test's rule)
    // INVERT: the predicate over a live CT32 target with the identity-invert CLUT.
    const uint32_t cbp = 0x3F0000;
    WriteInvertClut(w.vram, cbp);
    w.pipe.Draw(Raw(), nullptr);   // the CT32 target at 0x70000 the invert reads back
    DrawIr inv = InvertIr(cbp);
    CHECK(Profile().Shape().composite_invert.Matches(inv), "invert shape");
    CHECK(s9->Accept(inv), "invert accepted: live CT32 target, copy, content, this frame, CLUT ok");
    const int draws3 = w.be.draws;
    CHECK(w.pipe.Draw(inv, nullptr) && w.be.draws == draws3 + 1 && w.be.last_params.program == render::StencilProgram::Invert, "invert native: one Invert draw");
    CHECK(s9->Count().inverts == 1, "inverts counted");
    // Decline causes, one each.
    { DrawIr n = inv; n.raster.frame.fbp = n.tex.tbp = 0x3A0000; CHECK(!s9->Accept(n) && s9->Count().inv_no_target == 1, "no target"); }
    { DrawIr n = inv; n.raster.frame.fbp = n.tex.tbp = 0x230000; n.raster.frame.fbw = n.tex.tbw = 128; CHECK(!s9->Accept(n) && s9->Count().inv_psm == 1, "CT16S target: psm"); }
    { DrawIr n = inv; n.tex.cbp = 0x230000; CHECK(!s9->Accept(n) && s9->Count().inv_pending == 1, "deferred writeback under the CLUT: pending"); }
    { uint8_t* v = w.vram.MutableBytes(); const uint32_t a = PsmLayout::PixelByteAddress(Psm::CT32, 8, 0, cbp, 64) % VramImage::kBytes;
      v[a + 3] ^= 1; CHECK(!s9->Accept(inv) && s9->Count().inv_clut == 1, "one flipped CLUT byte: clut"); v[a + 3] ^= 1; }
    { DrawIr n = inv; n.verts[0].r = 0x81; CHECK(!s9->Accept(n), "vertex colour off the pin declines before the predicate"); }
    { DrawIr n = inv; n.tex.tbp = 0x71000; CHECK(!s9->Accept(n), "tbp != fbp declines"); }
    // The self-read pins decline BEFORE the predicate (OLD s9_composite.h:262: TBW == FBW): no decline cause counted.
    { DrawIr n = inv; n.tex.tbw = 128; const Counters c0 = s9->Count();
      CHECK(!s9->Accept(n) && c0.inv_no_target == s9->Count().inv_no_target && c0.inv_psm == s9->Count().inv_psm &&
            c0.inv_pending == s9->Count().inv_pending && c0.inv_clut == s9->Count().inv_clut, "tbw != fbw declines before the predicate"); }
    // The predicate's PSM arm (OLD :702): the colour-authored PSMZ32 copy (0x30) is a 32-bit-class target -> ACCEPTED;
    // CT16 (0x02) declines with `psm` like CT16S above (the 0x30 target at 0x268000 was drawn this frame, first draw).
    { DrawIr n = inv; n.raster.frame.fbp = n.tex.tbp = 0x268000; const RenderTarget* zt = w.cache.Find(0x268000);
      CHECK(zt && zt->identity.render_psm == 0x30, "the 0x268000 target carries render_psm 0x30");
      CHECK(s9->Accept(n) && s9->Count().inv_psm == 1, "render_psm 0x30 target: accepted by the predicate"); }
    { DrawIr ct16 = Raw(); ct16.raster.frame.fbp = 0x2A0000; ct16.raster.frame.psm = 0x02; w.pipe.Draw(ct16, nullptr);
      DrawIr n = inv; n.raster.frame.fbp = n.tex.tbp = 0x2A0000;
      CHECK(w.cache.Find(0x2A0000) && w.cache.Find(0x2A0000)->identity.render_psm == 0x02 && !s9->Accept(n) && s9->Count().inv_psm == 2,
            "CT16 (0x02) target: psm"); }
    // Emit: the reference grammar for this synthetic run.
    char buf[1024] = {0}; std::FILE* f = fmemopen(buf, sizeof(buf) - 1, "w"); s9->Emit(f); std::fclose(f);
    const char* want = "[GS-S9RDR] on=1 clears=3 primes=3 arms=2 strips_stencil=4 strips_color=3 inverts=1 materializes=2 mat_draws=64 demotes=1 scale_declines=0 dedupe_draws=8\n"
                       "[GS-S9RDR] invert_declines no_target=1 no_tex=0 no_content=0 stale=0 psm=2 pending=1 clut=1\n";
    CHECK(!std::strcmp(buf, want), "S9 emit grammar");
    if (std::strcmp(buf, want)) std::printf("--- got:\n%s--- want:\n%s", buf, want);
    // Without the sequence the profile registers six slots and the world strip is the plain generic.
    World w2;
    CHECK(w2.profile.RegisterClassRenderers(w2.registry) == 6 && w2.registry.Slot(C::kStencilComposite) == nullptr, "no sequence: six slots");
}
static void TestS9Steer() {
    World w;
    w.profile.RegisterClassRenderers(w.registry, &w.sequence);
    auto* s9 = dynamic_cast<StencilCompositeRenderer*>(w.registry.Slot(C::kStencilComposite));
    w.registry.WarmUpAll(w.pipe, nullptr);
    DrawIr zc = Raw(); zc.raster.frame.fbp = 0x268000; zc.raster.frame.psm = 0x30; w.pipe.Draw(zc, nullptr);
    // The raw fan family is accepted only with the world slot registered + enabled.
    DrawIr fan = S9Strip(0x08, 0, 0, 32, 32); fan.prim_type = 5;
    CHECK(Profile().Shape().composite_fan.Matches(fan) && s9->Accept(fan), "fan accepted with the world slot");
    w.registry.SetEnabled(C::kWorldTristrip, false);
    CHECK(!s9->Accept(fan), "fan declined when the world class is disabled");
    w.registry.SetEnabled(C::kWorldTristrip, true);
    // Disabled S9 slot: the world renderer's strip is the plain generic draw (no FSM change).
    w.pipe.Draw(ClearIr(), nullptr);   // fb_disabled would skip Accept; do it while enabled...
    CHECK(w.sequence.GetState() == SequenceRecipe::kPrimed, "primed");
    w.registry.SetEnabled(C::kStencilComposite, false);
    const size_t before = w.be.stencil_calls.size();
    w.pipe.Draw(S9Strip(0x08, 0, 0, 32, 32), nullptr);
    CHECK(w.be.stencil_calls.size() == before && s9->Count().strips_stencil == 0 && s9->Count().strips_color == 0, "S9 disabled: the steer short-circuits to the generic strip");
    w.registry.SetEnabled(C::kStencilComposite, true);
    // The colour arm has no pre-draw hook: an ARMED sequence on ANOTHER buffer stays armed
    // across a colour-path strip that would otherwise be a leaving-capture trigger.
    w.sequence.Idle();
    w.pipe.Draw(ClearIr(), nullptr); w.pipe.Draw(S9Strip(0x08, 0, 0, 32, 32), nullptr);
    CHECK(w.sequence.GetState() == SequenceRecipe::kArmed && w.pipe.ActiveTargetFbp() == 0x230000, "armed on the bound accumulator");
    DrawIr other = S9Strip(0x10, 0, 0, 32, 32); other.raster.frame.fbp = 0x70000;
    w.pipe.Draw(other, nullptr);
    CHECK(w.sequence.GetState() == SequenceRecipe::kArmed && s9->Count().strips_color == 1, "colour-path strip into another buffer: no hook, still armed");
    // ... while the SAME strip through the generic hook path (no S9) would have materialised.
    World w2; World::RecordingMaterializer mat; w2.pipe.SetMaterializer(&mat);
    w2.pipe.Draw(ClearIr(), nullptr); w2.sequence.Prime(0x230000); w2.sequence.Arm(false);
    w2.pipe.Draw(other, nullptr);
    CHECK(mat.calls == 1, "control: the hooked generic path materialises on leaving");
    // The alias abort: CPU-dirty pages under the 16-bit frame -> the ladder aborts before the
    // import, an armed sequence materialises, the colour path runs (and imports).
    DrawIr s = S9Strip(0x08, 0, 0, 32, 32);
    CHECK(w.sequence.GetState() == SequenceRecipe::kArmed, "still armed");
    w.pipe.OnInvalidate(0x230000, 0x2000);
    const int draws = w.be.draws;
    w.pipe.Draw(s, nullptr);
    CHECK(w.sequence.GetState() == SequenceRecipe::kIdle && s9->Count().materializes == 1 && s9->Count().strips_color == 2 &&
          w.be.draws == draws + 1 && w.be.last_params.program == render::StencilProgram::None, "alias demand: materialise + colour path");
    // PRIMED + alias demand -> idle (the !ladder_clean arm), colour path, no materialise; the
    // colour path's ladder performs the import (the aborted ladder ran none).
    w.pipe.Draw(ClearIr(), nullptr);
    w.pipe.OnInvalidate(0x230000, 0x2000);
    const int up0 = w.be.uploads;
    w.pipe.Draw(s, nullptr);
    const int up_s9 = w.be.uploads - up0;
    CHECK(w.sequence.GetState() == SequenceRecipe::kIdle && s9->Count().materializes == 1 && s9->Count().demotes == 0 && s9->Count().strips_color == 3, "primed + alias demand: idles without a demote count");
    w2.sequence.Idle(); w2.pipe.Draw(ClearIr(), nullptr); w2.pipe.OnInvalidate(0x230000, 0x2000);
    const int up1 = w2.be.uploads; w2.pipe.Draw(s, nullptr);
    CHECK(up_s9 == w2.be.uploads - up1 && !w.cache.AliasPages().AnyCpuDirty(0x230000, 0x2000),
          "alias abort: the colour path's ladder imported and cleared the CPU-dirty pages like the plain generic strip");
    // The STAMPED world strip (the corpus form) reaches the ladder through the world slot's steer.
    World w3;
    w3.profile.RegisterClassRenderers(w3.registry, &w3.sequence);
    auto* s9c = dynamic_cast<StencilCompositeRenderer*>(w3.registry.Slot(C::kStencilComposite));
    w3.registry.WarmUpAll(w3.pipe, nullptr);
    w3.pipe.Draw(zc, nullptr); w3.pipe.Draw(ClearIr(), nullptr);
    DrawIr stamped = S9Strip(0x08, 0, 0, 32, 32); stamped.identity.hle_handler = 7;
    CHECK(w3.pipe.Draw(stamped, "vs_family_tristrip"), "stamped strip native");
    CHECK(w3.registry.Count().native[C::kWorldTristrip] == 1 && w3.registry.Count().seen[C::kStencilComposite] == 1, "stamped strip counted against the world class (the CLEAR against S9)");
    CHECK(w3.sequence.GetState() == SequenceRecipe::kArmed && s9c->Count().arms == 1 && s9c->Count().strips_stencil == 1 &&
          w3.be.last_params.program == render::StencilProgram::StripCount, "world steer: the stamped strip armed and counted in the stencil");
    w3.registry.Register(C::kStencilComposite, nullptr);   // slot cleared: the steer is inert
    w3.pipe.Draw(stamped, "vs_family_tristrip");
    CHECK(w3.be.last_params.program == render::StencilProgram::None && s9c->Count().strips_stencil == 1 && s9c->Count().strips_color == 0, "S9 slot cleared: the world strip is the plain generic");
    // The inactive steer IS the colour path: no pre-draw hook, so a sequence armed on
    // another buffer survives a strip that would be a leaving-capture trigger.
    World w4;
    w4.profile.RegisterClassRenderers(w4.registry, &w4.sequence);
    auto* s9d = dynamic_cast<StencilCompositeRenderer*>(w4.registry.Slot(C::kStencilComposite));
    w4.registry.WarmUpAll(w4.pipe, nullptr);
    w4.pipe.Draw(zc, nullptr); w4.pipe.Draw(ClearIr(), nullptr); w4.pipe.Draw(S9Strip(0x08, 0, 0, 32, 32), nullptr);
    CHECK(w4.sequence.GetState() == SequenceRecipe::kArmed && w4.pipe.ActiveTargetFbp() == 0x230000, "armed on the bound accumulator (w4)");
    w4.registry.SetEnabled(C::kStencilComposite, false);
    DrawIr other_stamped = other; other_stamped.identity.hle_handler = 7;
    CHECK(w4.pipe.Draw(other_stamped, "vs_family_tristrip"), "stamped strip: the world slot draws it");
    CHECK(w4.sequence.GetState() == SequenceRecipe::kArmed && s9d->Count().materializes == 0 && s9d->Count().strips_color == 0,
          "inactive steer: the generic strip runs without the hook (still armed, uncounted)");
    // ... whereas a RAW strip is the S9 class itself: disabled -> the hooked generic fallback (a draw
    // into the armed accumulator materialises).
    w4.pipe.Draw(S9Strip(0x10, 0, 0, 32, 32), nullptr);
    CHECK(w4.sequence.GetState() == SequenceRecipe::kIdle && s9d->Count().materializes == 1 && s9d->Count().strips_color == 0,
          "raw strip under a disabled S9 slot: the hooked fallback materialises, the steer never ran");
    // A CLEAR over CPU-dirty accumulator pages still primes: its FBMSK is 0, so the
    // alias pre-draw block never enters (the abort arm is unreachable, as in OLD).
    World w5;
    w5.profile.RegisterClassRenderers(w5.registry, &w5.sequence);
    auto* s9e = dynamic_cast<StencilCompositeRenderer*>(w5.registry.Slot(C::kStencilComposite));
    w5.registry.WarmUpAll(w5.pipe, nullptr);
    w5.pipe.Draw(ClearIr(), nullptr);
    w5.pipe.OnInvalidate(0x230000, 0x2000);
    const int up5 = w5.be.uploads;
    w5.pipe.Draw(ClearIr(), nullptr);
    CHECK(w5.sequence.GetState() == SequenceRecipe::kPrimed && s9e->Count().clears == 2 && s9e->Count().primes == 2 &&
          w5.be.uploads == up5 && s9e->Demotes() == 1, "CLEAR under dirty pages: no alias import (FBMSK 0), the hook demotes the primed run, re-primes, counted");
}

// The load_transition-shaped run (8 composites, no volume strips, a zero CLUT
// mirror): every CLEAR primes, an RGB draw into the accumulator demotes, every
// invert declines on the CLUT walk -- the two counter lines 6F expects verbatim.
static void TestS9Emit() {
    World w;
    w.profile.RegisterClassRenderers(w.registry, &w.sequence);
    auto* s9 = dynamic_cast<StencilCompositeRenderer*>(w.registry.Slot(C::kStencilComposite));
    w.registry.WarmUpAll(w.pipe, nullptr);
    w.pipe.Draw(Raw(), nullptr);   // the CT32 target the invert would read back (live, drawn this frame)
    DrawIr rgb = Raw(); rgb.raster.frame.fbp = 0x230000; rgb.raster.frame.psm = 0x0A; rgb.raster.frame.fbw = 128;
    rgb.raster.scissor = DrawIr::Scissor{0, 0, 127, 127};
    DrawIr inv = InvertIr(0x3F0000);   // the CLUT bytes are the zero mirror
    for (int i = 0; i < 8; i++) {
        CHECK(w.pipe.Draw(ClearIr(), nullptr) && w.sequence.GetState() == SequenceRecipe::kPrimed, "composite CLEAR primes");
        CHECK(!w.pipe.Draw(rgb, nullptr) && w.sequence.GetState() == SequenceRecipe::kIdle, "RGB write into the primed accumulator demotes (generic)");
        CHECK(!w.pipe.Draw(inv, nullptr), "invert over the zero CLUT mirror: declined to the generic");
    }
    CHECK(s9->Demotes() == 8 && s9->Count().demotes == 0 && s9->Count().inv_clut == 8 && s9->Count().inverts == 0 && s9->Count().arms == 0,
          "8 hook demotes (none from the steer), 8 clut declines, no invert, no arm");
    CHECK(w.registry.Count().native[C::kStencilComposite] == 8 && w.registry.Count().fb_declined[C::kStencilComposite] == 16,
          "CLASSRDR: S9-shadow native=8 declined=16 (the CT16S RGB draws + the inverts)");
    char buf[1024] = {0}; std::FILE* f = fmemopen(buf, sizeof(buf) - 1, "w"); s9->Emit(f); std::fclose(f);
    const char* want = "[GS-S9RDR] on=1 clears=8 primes=8 arms=0 strips_stencil=0 strips_color=0 inverts=0 materializes=0 mat_draws=0 demotes=8 scale_declines=0 dedupe_draws=0\n"
                       "[GS-S9RDR] invert_declines no_target=0 no_tex=0 no_content=0 stale=0 psm=0 pending=0 clut=8\n";
    CHECK(!std::strcmp(buf, want), "S9 emit: the load_transition lines");
    if (std::strcmp(buf, want)) std::printf("--- got:\n%s--- want:\n%s", buf, want);
    // The profile's emitter prints S9 first, before the sprite renderer's line.
    char pb[2048] = {0}; std::FILE* pf = fmemopen(pb, sizeof(pb) - 1, "w"); w.profile.EmitRendererCounters(pf); std::fclose(pf);
    const char* s9pos = std::strstr(pb, "[GS-S9RDR]"); const char* sprpos = std::strstr(pb, "[GS-SPRRDR]");
    CHECK(s9pos == pb && (!sprpos || s9pos < sprpos), "profile emit order: [GS-S9RDR] first, before [GS-SPRRDR]");
    // Nothing seen: the emitter prints nothing (OLD's total == 0 gate).
    World w0;
    w0.profile.RegisterClassRenderers(w0.registry, &w0.sequence);
    auto* q = dynamic_cast<StencilCompositeRenderer*>(w0.registry.Slot(C::kStencilComposite));
    char buf0[64] = {0}; std::FILE* f0 = fmemopen(buf0, sizeof(buf0) - 1, "w"); q->Emit(f0); std::fclose(f0);
    CHECK(buf0[0] == 0, "no S9 draws: silent");
}
// The ownership rule (Review 6E'' item 1): the profile-owned renderer never calls
// the backend it does not own from its destructor; the stencil attachment is the
// device's to release at Shutdown, or the owner's through ReleaseBackendResources.
static void ClearSlots(World& w) {
    for (int i = 0; i < C::kClassCount; i++) w.registry.Register(static_cast<uint8_t>(i), nullptr);
    w.pipe.SetMaterializer(nullptr);
}
static void TestS9Lifetime() {
    // (a) renderer destroyed while its backend is alive: no DestroySurface, the surface stays.
    {
        World w;
        render::SurfaceId sid; int destroys0 = -1;
        {
            title::RendererProfile inner;
            inner.RegisterClassRenderers(w.registry, &w.sequence);
            auto* s9 = dynamic_cast<StencilCompositeRenderer*>(w.registry.Slot(C::kStencilComposite));
            w.registry.WarmUpAll(w.pipe, nullptr);
            DrawIr zc = Raw(); zc.raster.frame.fbp = 0x268000; zc.raster.frame.psm = 0x30; w.pipe.Draw(zc, nullptr);
            w.pipe.Draw(ClearIr(), nullptr); w.pipe.Draw(S9Strip(0x08, 0, 0, 64, 64), nullptr);
            sid = s9->StencilSurface();
            CHECK(sid.id != 0 && w.be.Has(sid) && w.sequence.GetState() == SequenceRecipe::kArmed, "armed: the stencil attachment exists");
            destroys0 = w.be.destroys;
        }   // `inner` (and its renderers) destroyed here, `w.be` alive
        CHECK(w.be.destroys == destroys0 && w.be.Has(sid), "destructor made no backend call: the stencil surface is still the device's");
        ClearSlots(w);   // the dangling slots / materializer are the owner's to drop
    }
    // (b) the explicit path: ReleaseBackendResources frees the attachment through the live backend and the renderer forgets it.
    {
        World w;
        w.profile.RegisterClassRenderers(w.registry, &w.sequence);
        auto* s9 = dynamic_cast<StencilCompositeRenderer*>(w.registry.Slot(C::kStencilComposite));
        w.registry.WarmUpAll(w.pipe, nullptr);
        DrawIr zc = Raw(); zc.raster.frame.fbp = 0x268000; zc.raster.frame.psm = 0x30; w.pipe.Draw(zc, nullptr);
        w.pipe.Draw(ClearIr(), nullptr); w.pipe.Draw(S9Strip(0x08, 0, 0, 64, 64), nullptr);
        const render::SurfaceId sid = s9->StencilSurface();
        const int d0 = w.be.destroys;
        w.profile.ReleaseBackendResources(w.be);
        CHECK(w.be.destroys == d0 + 1 && !w.be.Has(sid) && s9->StencilSurface().id == 0, "Release: one DestroySurface through the given backend, handle forgotten");
        CHECK(!s9->Accept(InvertIr(0x3F0000)) && s9->Count().inv_no_target == 1, "released renderer: no cached pipeline until the next draw (no_target)");
        // Still registered and usable: the next strip re-creates the attachment on the same backend.
        const int c0 = w.be.creates;
        w.pipe.Draw(S9Strip(0xF8, 0, 0, 32, 32), nullptr);   // ARMED + dims changed (0) -> materialise + colour, idle
        w.pipe.Draw(ClearIr(), nullptr); w.pipe.Draw(S9Strip(0x08, 0, 0, 64, 64), nullptr);
        CHECK(w.be.creates == c0 + 1 && s9->StencilSurface().id != 0 && w.be.Has(s9->StencilSurface()), "re-armed: one new stencil surface on the live backend");
    }
    // (c) a profile outliving its World: the profile outlives the World it was registered against, then is re-registered
    // against a fresh World -- the replaced renderers die without a backend call (no backend to call), the new set works.
    {
        title::RendererProfile outer;
        {
            World w;
            outer.RegisterClassRenderers(w.registry, &w.sequence);
            auto* s9 = dynamic_cast<StencilCompositeRenderer*>(w.registry.Slot(C::kStencilComposite));
            w.registry.WarmUpAll(w.pipe, nullptr);
            DrawIr zc = Raw(); zc.raster.frame.fbp = 0x268000; zc.raster.frame.psm = 0x30; w.pipe.Draw(zc, nullptr);
            w.pipe.Draw(ClearIr(), nullptr); w.pipe.Draw(S9Strip(0x08, 0, 0, 64, 64), nullptr);
            CHECK(s9->StencilSurface().id != 0, "stencil created against the first World");
        }   // the World (backend, pipeline, registry) is gone; `outer` still holds the renderers
        World w2;
        CHECK(outer.RegisterClassRenderers(w2.registry, &w2.sequence) == 7, "re-registered against a fresh World (old set replaced, no backend call)");
        auto* s9 = dynamic_cast<StencilCompositeRenderer*>(w2.registry.Slot(C::kStencilComposite));
        w2.registry.WarmUpAll(w2.pipe, nullptr);
        DrawIr zc = Raw(); zc.raster.frame.fbp = 0x268000; zc.raster.frame.psm = 0x30; w2.pipe.Draw(zc, nullptr);
        w2.pipe.Draw(ClearIr(), nullptr); w2.pipe.Draw(S9Strip(0x08, 0, 0, 64, 64), nullptr);
        CHECK(w2.sequence.GetState() == SequenceRecipe::kArmed && w2.be.Has(s9->StencilSurface()), "the new set arms on the new backend");
        outer.ReleaseBackendResources(w2.be);   // the correct teardown: release before the backend goes
        CHECK(s9->StencilSurface().id == 0, "released before the backend dies");
        ClearSlots(w2);
    }   // `outer` destroyed after `w2`: nothing to call
}

int main(int argc, char** argv) {
    const std::string sel = argc > 1 ? argv[1] : "all";
    if (sel == "classify" || sel == "all") TestClassify();
    if (sel == "dispatch" || sel == "all") TestDispatch();
    if (sel == "accept" || sel == "all") TestAccept();
    if (sel == "livesnap" || sel == "all") TestLiveSnapshot();
    if (sel == "sequence" || sel == "all") TestSequence();
    if (sel == "snap" || sel == "all") TestSnap();
    if (sel == "pipeline" || sel == "all") TestPipeline();
    if (sel == "hooks" || sel == "all") TestHooks();
    if (sel == "grammar" || sel == "all") TestGrammar();
    if (sel == "profile" || sel == "all") TestProfile();
    if (sel == "s9passes" || sel == "all") TestS9Passes();
    if (sel == "s9steer" || sel == "all") TestS9Steer();
    if (sel == "s9emit" || sel == "all") TestS9Emit();
    if (sel == "s9lifetime" || sel == "all") TestS9Lifetime();
    std::printf("test_classrdr %s: %d checks, %d failures\n", sel.c_str(), g_checks, g_fails);
    return g_fails ? 1 : 0;
}
