// tests/render/test_null_backend.cpp -- Render_NullBackend: the backend that rasterises nothing honours the contract
// in shape: surfaces are described after creation and gone after destruction, a draw of three or more vertices reports
// drew with the vertices' bounding boxes and the scissor-clipped write rect, a shorter one refuses, readbacks hand back
// zero bytes in the attachment's layout, the stencil mark succeeds only over live surfaces, a present of a missing
// source refuses, and every call is counted. No GL.
#include <cstdio>
#include <vector>

#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/texsrc/texture_source.h"
#include "engine/render/backends/null/null_backend.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

using namespace ps2r;

int main() {
    render::NullBackend b;
    render::SurfaceDesc cd;
    cd.width = 64;
    cd.height = 32;
    cd.renderable = true;
    const render::SurfaceId color = b.CreateSurface(cd);
    render::SurfaceDesc dd = cd;
    dd.format = render::SurfaceFormat::DepthStencil24_8;
    dd.renderable = false;
    const render::SurfaceId depth = b.CreateSurface(dd);
    CHECK(color.id != 0 && depth.id != 0 && color.id != depth.id, "two surfaces created with distinct ids");
    CHECK(b.CreateSurface(render::SurfaceDesc{}).id == 0, "a zero-sized surface is refused");
    CHECK(b.DescribeSurface(color).width == 64 && b.DescribeSurface(color).renderable, "the colour surface is described");
    CHECK(b.DescribeSurface(depth).format == render::SurfaceFormat::DepthStencil24_8, "the depth-stencil surface is described");
    CHECK(b.SurfacesAlive() == 2, "two alive");

    b.Clear(color, depth, 0xFF000000u, 0.f, render::kClearColor | render::kClearDepth);
    std::vector<uint8_t> px(64 * 4, 0xAB);
    b.Upload(color, render::Rect{0, 0, 64, 1}, px.data(), 0);
    CHECK(b.Stats().clears == 1 && b.Stats().uploads == 1, "clear and upload counted");

    // A triangle list of 4 vertices: 3 complete + 1 tail; scissor 0..31 x 0..31; target 64x32.
    gs::DrawIr ir;
    ir.verts.resize(4);
    for (auto& v : ir.verts) v = gs::VertexRec{};
    ir.verts[0].x = 2.f;  ir.verts[0].y = 3.f;
    ir.verts[1].x = 40.f; ir.verts[1].y = 3.f;
    ir.verts[2].x = 2.f;  ir.verts[2].y = 20.f;
    ir.verts[3].x = 60.f; ir.verts[3].y = 30.f;
    ir.raster.scissor = {0, 0, 31, 31};
    gs::TextureSource ts;
    render::DrawParams p;
    p.color = color;
    p.depth = depth;
    p.render_w = p.target_w = 64;
    p.render_h = p.target_h = 32;
    p.stencil_state.enabled = true;
    p.stencil_state.depth_fail_op = render::StencilState::kIncrWrap;
    const render::DrawResult r = b.Draw(ir, ts, p);
    CHECK(r.drew && r.bbox_valid, "a 4-vertex draw draws");
    CHECK(r.bbox_tri[0] == 2.f && r.bbox_tri[1] == 3.f && r.bbox_tri[2] == 40.f && r.bbox_tri[3] == 20.f, "the triangle box");
    CHECK(r.bbox_all[2] == 60.f && r.bbox_all[3] == 30.f, "the box over the tail");
    CHECK(r.write_rect.x == 2 && r.write_rect.y == 3 && r.write_rect.w == 30 && r.write_rect.h == 27, "write rect = box within the scissor (x clipped at 32, y at the tail's 30)");
    CHECK(b.Stats().draws == 1 && b.Stats().triangles == 1, "one draw, one triangle counted");
    ir.verts.resize(2);
    CHECK(!b.Draw(ir, ts, p).drew && b.Stats().draws_refused == 1, "a 2-vertex draw refuses");
    render::DrawParams none = p;
    none.color = render::SurfaceId{};
    ir.verts.resize(3);
    CHECK(!b.Draw(ir, ts, none).drew && b.Stats().draws_refused == 2, "a draw into no colour surface refuses");

    std::vector<uint8_t> out(64 * 32 * 4, 0xCD);
    b.Readback(color, render::Rect{0, 0, 64, 32}, out.data(), 0);
    bool zero = true;
    for (uint8_t v : out) zero = zero && v == 0;
    CHECK(zero, "a readback hands back zero bytes");
    std::vector<uint8_t> strided(2 * 300, 0xCD);
    b.Readback(color, render::Rect{0, 0, 8, 2}, strided.data(), 300);
    CHECK(strided[0] == 0 && strided[31] == 0 && strided[32] == 0xCD && strided[300] == 0 && strided[331] == 0 && strided[332] == 0xCD,
          "a strided readback zeroes the rows only");
    const render::FenceId fence = b.BeginReadback(color, render::Rect{0, 0, 4, 1});
    std::vector<uint8_t> staged(16, 0xCD);
    CHECK(fence.id != 0 && b.PollReadback(fence, staged.data(), 0) && staged[15] == 0, "the staging pair completes with zeros");
    CHECK(!b.PollReadback(fence, staged.data(), 0), "a completed fence is gone");

    b.ClearStencil(depth, 15);
    CHECK(b.MarkStencilWhereOpaque(color, depth, 0x20), "the stencil mark over live surfaces succeeds");
    CHECK(!b.MarkStencilWhereOpaque(color, render::SurfaceId{}, 0x20), "the stencil mark over no stencil refuses");
    uint8_t sten[4] = {1, 2, 3, 4};
    CHECK(!b.ReadbackStencil(depth, render::Rect{0, 0, 2, 2}, sten) && sten[0] == 1, "no stencil readback; the buffer untouched");
    b.DrawConstQuad(color, depth, render::Rect{0, 0, 8, 8}, 0x80404040u, false, render::StencilState{});
    CHECK(b.Stats().stencil_clears == 1 && b.Stats().stencil_marks == 1 && b.Stats().const_quads == 1, "stencil work counted");

    b.SetWindowSize(640, 480);
    CHECK(b.WindowWidth() == 640 && b.WindowHeight() == 480, "the window size is kept");
    b.BeginFrame();
    b.BeginDrawRun();
    b.EndDrawRun();
    render::ScanoutDesc sd;
    sd.source = color;
    b.Present(sd);
    sd.source = render::SurfaceId{99};
    b.Present(sd);
    b.EndFrame();
    CHECK(b.Stats().presents == 1, "a present of a missing nonzero source refuses");
    CHECK(b.Stats().frames == 1 && b.Stats().draw_runs == 2, "the frame and its runs counted");
    std::vector<uint8_t> img;
    int w = 0, h = 0;
    CHECK(!b.ReadPresented(&img, &w, &h), "no window image is ever held");

    b.DestroySurface(color);
    b.DestroySurface(depth);
    CHECK(b.SurfacesAlive() == 0 && b.DescribeSurface(color).width == 0, "destroyed surfaces are gone");
    CHECK(b.Stats().surfaces_created == 2 && b.Stats().surfaces_destroyed == 2, "creates and destroys counted");

    std::printf("test_null_backend: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
