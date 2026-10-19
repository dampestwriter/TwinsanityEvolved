#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_fixture.h"

EffectsFixture::EffectsFixture() {
    pipeline.OnSetWindow(512, 448, 512, 448);
    pipeline.OnSetFrontBuffer(frame.color_buffer, 512, 0);
}
EffectsFixture::~EffectsFixture() {
    effects.ResetAfterQuiescence();
    scene.CollectAfterQuiescence();
    cache.Ledger().Remove(frame.color_buffer);
}
ps2r::gs::DrawIr EffectsFixture::OrdinaryDraw() const {
    ps2r::gs::DrawIr draw;
    draw.src_frame = frame.source_frame;
    draw.prim_type = 3;
    draw.raster.frame = {frame.color_buffer, frame.width, 0, 0};
    draw.raster.zbuf = {frame.depth_buffer, 0, false};
    draw.raster.test.zte = true;
    draw.raster.test.ztst = 2;
    draw.raster.scissor = {0, 0, uint16_t(frame.width - 1), uint16_t(frame.height - 1)};
    draw.verts.resize(3);
    draw.vertex_count = 3;
    draw.verts[1].x = 16;
    draw.verts[2].y = 16;
    return draw;
}
