// games/slus_20909_crash_twinsanity/renderer_profile/renderer_profile.cpp -- the
// VRAM layout of Crash Twinsanity (SLUS-20909) as observed in the GS corpus.
#include "games/slus_20909_crash_twinsanity/renderer_profile/renderer_profile.h"

namespace ps2r {
namespace title {

RendererProfile::RendererProfile() {
    gs::VramLayout& v = vram_;
    // The display buffer: FBP 56 (0x70000), 512 px wide, PSMCT32 -- the only
    // scanout configuration observed corpus-wide; also the FRAME of the FMV blit.
    v.main_fb_fbp = 0x70000;
    v.native_scanout_fbp_bytes = 0x70000;
    v.native_scanout_fbw_px = 512;
    v.native_scanout_psm = 0x00;
    // The scene accumulator (CT16S 256x224 sub-pass buffer / stencil composite
    // source): pinned to native scale, its render height follows SCISSOR, and a
    // non-indexed non-16-bit read of it serves the colour attachment directly.
    v.scene_accum_fbp = 0x230000;
    // The FMV blit reads its decoded frames from a CT32 1024x1024 texture here.
    v.fmv_stage_tbp = 0x150000;
    // Stencil recipe pass C: colour copy of the Z buffer.
    v.stencil_zcopy_fbp = 0x268000;
    // 16-bit fog strip lanes (index-lane keep mask writes; Z16 texture reads).
    v.fog_lane_a_fbp = 0x280000;
    v.fog_lane_b_fbp = 0x2F0000;
    // Reader band: the pages of the CLUT feedback band a texture read observes,
    // so a GPU deposit into them lands eagerly instead of deferring to present.
    static const uint16_t kBands[][2] = {
        {0x161, 0x163}, {0x169, 0x16B}, {0x171, 0x173}, {0x179, 0x17B}, {0x181, 0x183},
        {0x189, 0x18B}, {0x191, 0x193}, {0x199, 0x19B}, {0x1A0, 0x1A3}, {0x1A9, 0x1AB}};
    for (const auto& b : kBands)
        for (uint16_t p = b[0]; p <= b[1]; ++p) v.reader_band_pages.push_back(p);
    // Text-atlas CLUTs (tile atlas 0x380000.. stride 0x10000, CLUT at +0x8000) are
    // exempt from the CLUT-heal substitution.
    v.clut_heal_exempt_lo = 0x388000;
    v.clut_heal_exempt_hi = 0x3F8000;
    v.clut_heal_exempt_mask = 0xFFFF;
    v.clut_heal_exempt_value = 0x8000;
    FillRules();
    FillShapes();
    FillSequence();
}

// The VU1 HLE registry names of the nine stamped programs, matched as substrings
// in this order; the raw-shape signatures of the shadow composite (DATE strips,
// the CT16S accumulator FRAME, the PSMT8H self-read) and the fullscreen-blit
// extent (>= 256 x 128 px).
void RendererProfile::FillRules() {
    using C = gs::ClassClassifier;
    rules_.stamps = {
        {"skinned", C::kSkinned},
        {"family_tristrip", C::kWorldTristrip},
        {"textured_tristrip", C::kWorldTristrip},
        {"lit_tristrip", C::kWorldTristrip},
        {"poscolor", C::kParticlesUntex},
        {"indexed_displace", C::kIndexedDisplace},
        {"sprite_text", C::kSpritesHud},
    };
    rules_.composite_on_date = true;
    rules_.composite_frame_psm = 0x0A;   // PSMCT16S accumulator
    rules_.composite_tex_psm = 0x1B;     // PSMT8H self-read
    rules_.blit_min_w = 256.0f;
    rules_.blit_min_h = 128.0f;
}

// Every Accept shape the corpus census proved for its class, field for field.
void RendererProfile::FillShapes() {
    using S = gs::DrawShapeSpec;
    Shapes& s = shapes_;
    // World volume strips: flat untextured FST tristrips, wrap-blended (0,2,2,1)
    // with COLCLAMP off, DATE (DATM 0), Z GEQUAL with the Z write masked, into the
    // CT16S accumulator over a CT32 Z buffer, alpha byte masked.
    S& w = s.world_strip;
    w.prim = 4; w.gouraud = 0; w.use_fst = 1; w.fog = 0; w.tex_enabled = 0; w.shuffle = 0;
    w.alpha_blend = 1; w.abcd = {S::Abcd(0, 2, 2, 1)}; w.colclamp = 0; w.pabe = 0; w.fba = 0;
    w.ate = 0; w.date = 1; w.datm = 0; w.zte = 1; w.ztst = 2; w.zmsk = 1;
    w.frame_psm = 0x0A; w.zbuf_psm = 0x00; w.fbmsk = 0xff000000; w.scanmsk = 0;
    s.composite_fan = w;
    s.composite_fan.prim = 5;
    // Skinned mesh: gouraud STQ tristrips, PSMT8 (CT32 CLUT, CSM1) modulated with
    // alpha, REPEAT, linear mag / mip-nearest min, unblended, Z GEQUAL with writes,
    // into a CT32 frame with every bit written; an alpha test only as GEQUAL/KEEP.
    S& k = s.skinned;
    k.prim = 4; k.gouraud = 1; k.use_fst = 0; k.fog = 0; k.shuffle = 0; k.alpha_blend = 0;
    k.colclamp = 1; k.pabe = 0; k.atst_if_ate = 5; k.afail_if_ate = 0; k.date = 0;
    k.zte = 1; k.ztst = 2; k.zmsk = 0; k.frame_psm = 0x00; k.zbuf_psm = 0x00; k.fbmsk = 0;
    k.scanmsk = 0; k.tex_enabled = 1; k.tpsm = 0x13; k.tfx = 0; k.tcc = 1; k.cpsm = 0x00;
    k.csm = 0; k.csa = 0; k.wms = 0; k.wmt = 0; k.mmag = 1; k.mmin = 4;
    // HUD sprites: FBA sprites, depth ALWAYS with the Z write masked, into a CT32
    // frame with the alpha byte masked.
    S& c = s.sprite_common;
    c.prim = 6; c.fog = 0; c.shuffle = 0; c.pabe = 0; c.fba = 1; c.ate = 0; c.date = 0;
    c.zte = 1; c.ztst = 1; c.zmsk = 1; c.frame_psm = 0x00; c.zbuf_psm = 0x00;
    c.fbmsk = 0xff000000; c.scanmsk = 0;
    S& t = s.sprite_textured;   // the 128x256 PSMT8 text atlas, CLAMP, linear
    t.gouraud = 1; t.use_fst = 1; t.alpha_blend = 1; t.abcd = {S::Abcd(0, 1, 0, 1)}; t.colclamp = 1;
    t.tex_enabled = 1; t.tpsm = 0x13; t.tfx = 0; t.tcc = 1; t.cpsm = 0; t.csm = 0; t.csa = 0;
    t.tw = 7; t.th = 8; t.tbw = {128}; t.wms = 1; t.wmt = 1; t.mmag = 1; t.mmin = 1;
    S& u = s.sprite_untextured;
    u.gouraud = 0; u.use_fst = 0; u.alpha_blend = 0; u.colclamp = 1; u.tex_enabled = 0;
    // Particles: gouraud STQ tristrips over PSMT8 (REPEAT, linear), depth ALWAYS
    // with writes, into a CT32 frame; unblended or (0,1,0,1) / (0,2,0,1).
    S& p = s.particles;
    p.prim = 4; p.gouraud = 1; p.use_fst = 0; p.fog = 0; p.shuffle = 0;
    p.abcd = {S::Abcd(0, 1, 0, 1), S::Abcd(0, 2, 0, 1)}; p.colclamp = 1; p.pabe = 0; p.fba = 0;
    p.atst_if_ate = 5; p.afail_if_ate = 0; p.date = 0; p.zte = 1; p.ztst = 1; p.zmsk = 0;
    p.frame_psm = 0x00; p.zbuf_psm = 0x00; p.fbmsk = 0; p.scanmsk = 0; p.tex_enabled = 1;
    p.tpsm = 0x13; p.tfx = 0; p.tcc = 1; p.cpsm = 0; p.csm = 0; p.csa = 0; p.wms = 0; p.wmt = 0;
    p.mmag = 1; p.mmin = 1;
    // FMV blit: flat FST DECAL sprite of the CT32 1024x1024 stage (TBW 512 or 640,
    // REPEAT, nearest), unblended, Z GEQUAL with the write masked, alpha byte masked.
    S& f = s.fmv_blit;
    f.prim = 6; f.gouraud = 0; f.use_fst = 1; f.fog = 0; f.shuffle = 0; f.alpha_blend = 0;
    f.colclamp = 1; f.pabe = 0; f.fba = 0; f.ate = 0; f.date = 0; f.zte = 1; f.ztst = 2; f.zmsk = 1;
    f.frame_psm = 0x00; f.zbuf_psm = 0x00; f.fbmsk = 0xff000000; f.scanmsk = 0; f.tex_enabled = 1;
    f.tpsm = 0x00; f.tfx = 1; f.tcc = 1; f.tw = 10; f.th = 10; f.tbw = {512, 640}; f.wms = 0;
    f.wmt = 0; f.mmag = 0; f.mmin = 0;
    // Composite pass A: the accumulator CLEAR -- flat untextured FST sprite, no
    // blend, depth ALWAYS with the write masked, every bit of the CT16S frame written.
    S& a = s.composite_clear;
    a.prim = 6; a.gouraud = 0; a.use_fst = 1; a.fog = 0; a.tex_enabled = 0; a.shuffle = 0;
    a.alpha_blend = 0; a.colclamp = 1; a.pabe = 0; a.fba = 0; a.ate = 0; a.date = 0; a.zte = 1;
    a.ztst = 1; a.zmsk = 1; a.frame_psm = 0x0A; a.zbuf_psm = 0x00; a.fbmsk = 0; a.scanmsk = 0;
    // Composite pass D: the byte-NOT -- a flat FST sprite reading its own CT32
    // frame as PSMT8H through a CT32 CSM1 CLUT in HIGHLIGHT2 with alpha, blended
    // (0,1,2,2) FIX 0x80, COLCLAMP on, depth ALWAYS with the write masked, every
    // bit written. (The self-read TBP == FBP / TBW == FBW pins and the vertex
    // colour are checked by the renderer beyond the spec vocabulary.)
    S& d = s.composite_invert;
    d.prim = 6; d.gouraud = 0; d.use_fst = 1; d.fog = 0; d.tex_enabled = 1; d.tpsm = 0x1B;
    d.tfx = 3; d.tcc = 1; d.cpsm = 0x00; d.csm = 0; d.csa = 0; d.shuffle = 0; d.alpha_blend = 1;
    d.abcd = {S::Abcd(0, 1, 2, 2)}; d.fix = 0x80; d.colclamp = 1; d.pabe = 0; d.fba = 0; d.ate = 0;
    d.date = 0; d.zte = 1; d.ztst = 1; d.zmsk = 1; d.frame_psm = 0x00; d.zbuf_psm = 0x00;
    d.fbmsk = 0; d.scanmsk = 0;
}

// The decoded shadow-composite recipe: CLEAR to 0x78787878, so the stencil base
// is 0x78 >> 3 = 0x0F with a mod-32 counter (bit 4 of the counter is byte bit 7
// after the quantise pass); strips carry +0x080808 (front, +1) or +0xF8F8F8
// (back, -1); the invert pass pins vertex colour (0x80, 0x80, 0x80, 0x00).
void RendererProfile::FillSequence() {
    gs::SequenceRecipe::Constants& k = sequence_;
    k.clear_rgba = 0x78787878u;
    k.stencil_base = 0x0F;
    k.counter_mask = 0x1F;
    k.quant_bit = 0x10;
    k.incr_rgb[0] = k.incr_rgb[1] = k.incr_rgb[2] = 0x08;
    k.decr_rgb[0] = k.decr_rgb[1] = k.decr_rgb[2] = 0xF8;
    k.invert_rgba[0] = k.invert_rgba[1] = k.invert_rgba[2] = 0x80;
    k.invert_rgba[3] = 0x00;
    k.rgb_mask = 0x00FFFFFFu;
}

int RendererProfile::RegisterClassRenderers(gs::ClassRegistry& reg, gs::SequenceRecipe* sequence) {
    using C = gs::ClassClassifier;
    skinned_ = std::make_unique<gs::SkinnedRenderer>(shapes_.skinned, gs::SkinnedRenderer::kSkin,
                                                     &skin_stats_);
    raw_skin_ = std::make_unique<gs::SkinnedRenderer>(shapes_.skinned, gs::SkinnedRenderer::kRaw,
                                                      &skin_stats_);
    world_ = std::make_unique<gs::WorldTristripRenderer>(shapes_.world_strip, skinned_.get());
    sprites_ = std::make_unique<gs::SpriteHudRenderer>(shapes_.sprite_common, shapes_.sprite_textured,
                                                       shapes_.sprite_untextured);
    particles_ = std::make_unique<gs::ParticlesRenderer>(shapes_.particles);
    fmv_ = std::make_unique<gs::FmvBlitRenderer>(shapes_.fmv_blit);
    reg.Register(C::kWorldTristrip, world_.get());
    reg.Register(C::kSkinned, skinned_.get());
    reg.Register(C::kParticlesUntex, particles_.get());
    reg.Register(C::kSpritesHud, sprites_.get());
    reg.Register(C::kRawBlitFullscreen, fmv_.get());
    reg.Register(C::kRawOther, raw_skin_.get());
    if (!sequence) return 6;
    gs::StencilCompositeRenderer::Shapes s9;
    s9.clear = &shapes_.composite_clear;
    s9.strip = &shapes_.world_strip;
    s9.fan = &shapes_.composite_fan;
    s9.invert = &shapes_.composite_invert;
    stencil_ = std::make_unique<gs::StencilCompositeRenderer>(s9, *sequence, &reg);
    reg.Register(C::kStencilComposite, stencil_.get());
    world_->SetStencilComposite(stencil_.get());
    return 7;
}

void RendererProfile::ReleaseBackendResources(render::Backend& be) {
    if (stencil_) stencil_->Release(be);   // the only renderer holding a backend surface
}

void RendererProfile::EmitRendererCounters(std::FILE* f) const {
    if (stencil_) stencil_->Emit(f);   // the reference prints S9 before the sprite renderer
    if (sprites_) sprites_->Count().Emit(f);
    skin_stats_.Emit(f);
    if (particles_) particles_->Count().Emit(f);
    if (fmv_) fmv_->Count().Emit(f);
}

}  // namespace title
}  // namespace ps2r
