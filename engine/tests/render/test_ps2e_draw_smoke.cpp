// tests/render/test_ps2e_draw_smoke.cpp -- Render_Ps2eDrawSmoke: the C typed-draw surface end to end over a hidden
// window: a 128x96 frame cleared to a colour, one untextured red triangle (lower-left half), one textured 2x2
// checker quad (top-right quarter, nearest), presented with the capture armed; the presented image is read back
// top row first and the expected pixels are checked. Exit 77 (SKIP) without a GL context.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/platform/ps2e/ps2e_draw.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

int main() {
    ps2e_draw_config cfg{};
    cfg.frame_width = 128;
    cfg.frame_height = 96;
    cfg.headless = 1;
    cfg.title = "test_ps2e_draw_smoke";
    ps2e_draw* d = ps2e_draw_create(&cfg);
    if (!d) {
        std::printf("test_ps2e_draw_smoke: no GL context -> SKIP (77)\n");
        return 77;
    }
    ps2e_draw_arm_capture(d, 1);

    // A 2x2 checker: red / green / blue / white (PS2 alpha 0x80 = opaque).
    const uint8_t checker[16] = {255, 0, 0, 0x80, 0, 255, 0, 0x80, 0, 0, 255, 0x80, 255, 255, 255, 0x80};
    const uint32_t tex = ps2e_draw_texture_create(d, 2, 2, checker);
    CHECK(tex != 0, "texture created");

    ps2e_draw_clear clear{0x10, 0x20, 0x30, 1, 1};
    ps2e_draw_begin_frame(d, &clear);

    // Red triangle over the lower-left half of the frame: (0,0) (128,96) (0,96) in frame pixels (y down).
    ps2e_draw_state flat{};
    flat.gouraud = 1;
    flat.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
    flat.depth_write = 1;
    ps2e_draw_vertex tri[3] = {{0.f, 0.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f}, {128.f, 96.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f},
                               {0.f, 96.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f}};
    CHECK(ps2e_draw_submit(d, &flat, tri, 3) == 1, "triangle drawn");

    // The checker on the top-right quarter: x 64..128, y 0..48, u/v 0..1 (nearest, MODULATE by 0x80 = as is).
    ps2e_draw_state textured = flat;
    textured.texture = tex;
    textured.tfx = PS2E_DRAW_TFX_MODULATE;
    textured.tcc = 1;
    textured.wrap_u = textured.wrap_v = PS2E_DRAW_WRAP_CLAMP;
    auto V = [](float x, float y, float u, float v) { return ps2e_draw_vertex{x, y, 1.f, u, v, 0x80, 0x80, 0x80, 0x80, 0.f}; };
    ps2e_draw_vertex quad[6] = {V(64, 0, 0, 0), V(128, 0, 1, 0), V(64, 48, 0, 1),
                                V(128, 0, 1, 0), V(128, 48, 1, 1), V(64, 48, 0, 1)};
    CHECK(ps2e_draw_submit(d, &textured, quad, 6) == 2, "textured quad drawn");

    ps2e_draw_present(d);
    CHECK(ps2e_draw_frames_presented(d) == 1, "one frame presented");

    int w = 0, h = 0;
    ps2e_draw_read_presented(d, nullptr, 0, &w, &h);
    CHECK(w == 128 && h == 96, "presented image is the frame's size when headless");
    std::vector<uint8_t> img(size_t(w) * h * 4);
    CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "presented image read");
    auto px = [&](int x, int y) { return img.data() + (size_t(y) * w + x) * 4; };
    const uint8_t* clear_px = px(120, 60);   // right, below the quad, above the hypotenuse: neither draw
    CHECK(clear_px[0] == 0x10 && clear_px[1] == 0x20 && clear_px[2] == 0x30, "clear colour where nothing drew");
    const uint8_t* red = px(10, 80);         // lower-left: the triangle
    CHECK(red[0] > 200 && red[1] < 40 && red[2] < 40, "red triangle pixel");
    const uint8_t* tl = px(72, 8);           // checker texel (0,0): red
    const uint8_t* tr = px(120, 8);          // (1,0): green
    const uint8_t* bl = px(72, 40);          // (0,1): blue
    const uint8_t* br = px(120, 40);         // (1,1): white
    CHECK(tl[0] > 200 && tl[1] < 40, "checker red");
    CHECK(tr[1] > 200 && tr[0] < 40, "checker green");
    CHECK(bl[2] > 200 && bl[0] < 40, "checker blue");
    CHECK(br[0] > 200 && br[1] > 200 && br[2] > 200, "checker white");
    const uint8_t* top_left_untouched = px(40, 4);   // above the hypotenuse (4/96 < 40/128), left of the quad
    CHECK(top_left_untouched[0] == 0x10 && top_left_untouched[2] == 0x30, "frame top-left keeps the clear");

    ps2e_draw_counters c{};
    ps2e_draw_counts(d, &c);
    CHECK(c.draws == 2 && c.refused == 0 && c.textures_alive == 1, "counters");

    // A second frame without clearing keeps the pixels; a refused draw (2 vertices) is counted.
    ps2e_draw_clear keep{0, 0, 0, 0, 0};
    ps2e_draw_begin_frame(d, &keep);
    CHECK(ps2e_draw_submit(d, &flat, tri, 2) < 0, "two vertices refused");
    ps2e_draw_present(d);
    CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "second image read");
    CHECK(px(10, 80)[0] > 200, "kept frame still red");

#if defined(PS2E_DRAW_HAS_PRESENT_CT16)
    // The CT16 present (the retail dithered 16-bit copy): the clear (0x10 0x20 0x30) comes out quantised to five bits --
    // every channel a multiple of 255/31 to within rounding -- and the dither cells differ across a 4x4 block where the
    // clear's low bits sit between two levels (0x20 + d: d in -4..3 straddles the 0x20 step: 28..35 -> 3 or 4 of 31).
    ps2e_draw_set_present_ct16(d, 1, 0);
    CHECK(ps2e_draw_present_ct16(d) == 1, "CT16 present armed");
    ps2e_draw_begin_frame(d, &keep);
    ps2e_draw_present(d);
    CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "CT16 image read");
    {
        int levels_ok = 0, cells_lo = 0, cells_hi = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const uint8_t* p = px(40 + x, 4 + y);   // the undrawn clear
                bool ok = true;
                for (int ch = 0; ch < 3; ++ch) {
                    const int level = (p[ch] * 31 + 127) / 255;
                    const int back = (level * 255 + 15) / 31;
                    if (std::abs(back - p[ch]) > 1) ok = false;
                }
                levels_ok += ok;
                const int g = (p[1] * 31 + 127) / 255;
                if (g == 3) cells_lo++;
                if (g == 4) cells_hi++;
            }
        CHECK(levels_ok == 16, "CT16: every channel on a 5-bit level");
        CHECK(cells_lo > 0 && cells_hi > 0 && cells_lo + cells_hi == 16, "CT16: the dither matrix splits the green 0x20 between levels 3 and 4");
    }
    ps2e_draw_set_present_ct16(d, 0, 0);
    ps2e_draw_begin_frame(d, &keep);
    ps2e_draw_present(d);
    CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "plain image read");
    CHECK(px(40, 4)[1] == 0x20, "CT16 off: the clear's green is 0x20 again");
#endif

#if defined(PS2E_DRAW_HAS_CAPTURE_FRAME)
    // A picture resolved for the capture without a present: the clear's colour changed to red, the frame captured, the
    // presented count unchanged; the next present shows and counts as usual
    {
        const uint64_t before = ps2e_draw_frames_presented(d);
        ps2e_draw_clear red{0xC0, 0x00, 0x00, 1, 1};
        ps2e_draw_begin_frame(d, &red);
        ps2e_draw_capture_frame(d);
        CHECK(ps2e_draw_frames_presented(d) == before, "capture_frame presents nothing");
        CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "capture_frame: the resolved picture read");
        CHECK(px(40, 4)[0] == 0xC0 && px(40, 4)[1] == 0x00, "capture_frame: the capture holds the red clear");
        ps2e_draw_begin_frame(d, &keep);
        ps2e_draw_present(d);
        CHECK(ps2e_draw_frames_presented(d) == before + 1, "the present after a capture counts");
    }
#endif

#if defined(PS2E_DRAW_HAS_FBA_DATE) && defined(PS2E_DRAW_HAS_STENCIL)
    // The shadow bracket's caster exclusion: a grey receiver quad (alpha 0x7E, the lit world's) on the left
    // half, a grey caster quad (alpha 0x80, a skin's) on the right, both at GS depth 2^31; one volume over the whole frame
    // whose face lies behind both (z 2^30: the ordinary GEQUAL test fails, the count takes it) counted up with the
    // destination-alpha test; the composite darkens the receiver by a quarter of 0x84 and leaves the caster alone.
    // Without the test (the port's old counting) both halves darken. An FBA receiver (alpha 0x7E written with fba) is a
    // caster too.
    auto bracket = [&](int date, int fba_left, uint8_t& left, uint8_t& right) {
        ps2e_draw_clear black{0, 0, 0, 1, 1};
        ps2e_draw_begin_frame(d, &black);
        auto Q = [](float x0, float x1, float z, uint8_t a) {
            auto v = [&](float x, float y) { return ps2e_draw_vertex{x, y, z, 0, 0, 0xA0, 0xA0, 0xA0, a, 0.f}; };
            return std::vector<ps2e_draw_vertex>{v(x0, 0), v(x1, 0), v(x0, 96), v(x1, 0), v(x1, 96), v(x0, 96)};
        };
        ps2e_draw_state scene{};
        scene.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
        scene.depth_write = 1;
        scene.fba = fba_left;
        CHECK(ps2e_draw_submit(d, &scene, Q(0, 64, 2147483648.f, 0x7E).data(), 6) == 2, "receiver drawn");
        scene.fba = 0;
        CHECK(ps2e_draw_submit(d, &scene, Q(64, 128, 2147483648.f, 0x80).data(), 6) == 2, "caster drawn");
        ps2e_draw_state clearSt{};
        clearSt.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
        clearSt.no_colour_write = 1;
        clearSt.stencil = PS2E_DRAW_STENCIL_CLEAR;
        CHECK(ps2e_draw_submit(d, &clearSt, Q(0, 128, 0.f, 0x80).data(), 6) == 0, "counter cleared");
        ps2e_draw_state up{};
        up.depth_test = PS2E_DRAW_DEPTH_GEQUAL;
        up.depth_write = 0;
        up.no_colour_write = 1;
        up.stencil = PS2E_DRAW_STENCIL_COUNT_UP;
        up.dest_alpha_test = date;
        CHECK(ps2e_draw_submit(d, &up, Q(0, 128, 1073741824.f, 0x80).data(), 6) == 2, "volume counted");
        ps2e_draw_state composite{};
        composite.depth_test = PS2E_DRAW_DEPTH_ALWAYS;
        composite.depth_write = 0;
        composite.blend = 1;
        composite.blend_a = 2; composite.blend_b = 0; composite.blend_c = 2; composite.blend_d = 1;
        composite.blend_fix = 0x20;
        composite.stencil = PS2E_DRAW_STENCIL_INSIDE;
        auto quad = Q(0, 128, 0.f, 0x80);
        for (auto& v : quad) v.r = v.g = v.b = 0x84;
        CHECK(ps2e_draw_submit(d, &composite, quad.data(), 6) == 2, "composite drawn");
        ps2e_draw_present(d);
        CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "bracket image read");
        left = px(32, 48)[1];
        right = px(96, 48)[1];
        std::printf("bracket date %d fba %d: left %u right %u\n", date, fba_left, unsigned(left), unsigned(right));
    };
    uint8_t left = 0, right = 0;
    bracket(1, 0, left, right);
    CHECK(left < 0xA0 - 20 && left > 0xA0 - 45, "the receiver is darkened by a quarter of 0x84");
    CHECK(right == 0xA0, "the caster (alpha 0x80) is not darkened");
    bracket(0, 0, left, right);
    CHECK(left < 0xA0 - 20 && right < 0xA0 - 20, "without the destination-alpha test both halves darken");
    bracket(1, 1, left, right);
    CHECK(left == 0xA0 && right == 0xA0, "an FBA material is a caster whatever its alpha");
#endif

#if defined(PS2E_DRAW_HAS_PRESENTATION)
    // The presentation (TwinsanityEvolved). The default shows the frame's own pixels (the plain picture: the
    // 128x96 capture filled); 4:3 on a 128x96 frame is the same shape (no bars); 16:9 letterboxes it (128x72 at y 12:
    // bars of 12 rows top and bottom); a move of +16 frame pixels shifts the picture right; scale 2 renders the same
    // geometry into a 256x192 target and the capture stays 128x96 with the triangle where it was; the texture filter
    // override makes the checker's nearest sampling bilinear (a texel border pixel blends).
    auto frame_with_red_triangle = [&] {
        ps2e_draw_clear black{0, 0, 0, 1, 1};
        ps2e_draw_begin_frame(d, &black);
        ps2e_draw_vertex t[3] = {{0.f, 0.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f}, {128.f, 96.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f},
                                 {0.f, 96.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f}};
        ps2e_draw_submit(d, &flat, t, 3);
        ps2e_draw_present(d);
        CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "presentation image read");
    };
    ps2e_draw_presentation pres{};
    ps2e_draw_get_presentation(d, &pres);
    CHECK(pres.aspect == PS2E_DRAW_ASPECT_PIXEL && pres.scale == 0 && pres.window_mode == PS2E_DRAW_WINDOWED, "presentation defaults are the pre-S-Q3 picture");
    int ox = -1, oy = -1, ow = 0, oh = 0;
    ps2e_draw_last_output_rect(d, &ox, &oy, &ow, &oh);
    CHECK(ox == 0 && oy == 0 && ow == 128 && oh == 96, "default output rect is the whole capture");
    pres.aspect = PS2E_DRAW_ASPECT_16_9;
    ps2e_draw_set_presentation(d, &pres);
    frame_with_red_triangle();
    ps2e_draw_last_output_rect(d, &ox, &oy, &ow, &oh);
    CHECK(ox == 0 && oy == 12 && ow == 128 && oh == 72, "16:9 letterboxes the 4:3 frame: 128x72 at y 12");
    CHECK(px(10, 4)[0] == 0 && px(10, 4)[1] == 0 && px(10, 90)[0] == 0, "the bars above and below are black");
    CHECK(px(10, 70)[0] > 200, "the triangle shows inside the picture");
    pres.aspect = PS2E_DRAW_ASPECT_PIXEL;
    pres.offset_x = 16;
    ps2e_draw_set_presentation(d, &pres);
    frame_with_red_triangle();
    ps2e_draw_last_output_rect(d, &ox, &oy, &ow, &oh);
    CHECK(ox == 16 && oy == 0, "MoveDisplay +16 frame pixels moves the picture right");
    CHECK(px(4, 80)[0] == 0 && px(30, 80)[0] > 200, "the moved picture leaves the left edge black and the triangle shifted");
    pres.offset_x = 0;
    pres.scale = 2;
    ps2e_draw_set_presentation(d, &pres);
    frame_with_red_triangle();
    CHECK(w == 128 && h == 96, "scale 2: the capture stays the logical size");
    CHECK(px(10, 80)[0] > 200 && px(100, 10)[0] == 0, "scale 2: the triangle where it was");
    pres.scale = 1;
    pres.texture_filter = 2;   // bilinear override of the nearest checker
    ps2e_draw_set_presentation(d, &pres);
    {
        ps2e_draw_clear black{0, 0, 0, 1, 1};
        ps2e_draw_begin_frame(d, &black);
        ps2e_draw_submit(d, &textured, quad, 6);
        ps2e_draw_present(d);
        ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h);
        const uint8_t* mid = px(96, 24);   // the checker's centre: nearest gives one texel's colour, bilinear a blend of the four
        CHECK(mid[0] > 60 && mid[0] < 200 && mid[1] > 60 && mid[1] < 200, "bilinear override blends the checker's centre");
    }
    pres.texture_filter = 0;
    ps2e_draw_set_presentation(d, &pres);
#endif

#if defined(PS2E_DRAW_HAS_FRAME_COPY)
    // The screen copy (TwinsanityEvolved). A frame-copy draw before any copy is refused; after ps2e_draw_copy_frame a
    // quad over the whole frame sampling the copy at (x / W, y / H) reproduces the frame (identity: the copy is the frame's
    // orientation); the same quad with u shifted by half the frame moves the red triangle half a frame left; a copy
    // taken after a draw holds it, nothing drawn after the copy is in it (the copy is ordered with the submits).
    {
        ps2e_draw_clear black{0, 0, 0, 1, 1};
        ps2e_draw_begin_frame(d, &black);
        ps2e_draw_vertex t[3] = {{0.f, 0.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f}, {64.f, 0.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f},
                                 {0.f, 48.f, 1.f, 0, 0, 255, 0, 0, 0x80, 0.f}};   // the top-left quarter's upper triangle
        ps2e_draw_submit(d, &flat, t, 3);
        ps2e_draw_state copyState = textured;
        copyState.texture = PS2E_DRAW_TEXTURE_FRAME_COPY;
        copyState.wrap_u = copyState.wrap_v = PS2E_DRAW_WRAP_CLAMP;
        copyState.linear = 0;
        auto whole = [](float du) {
            std::array<ps2e_draw_vertex, 6> q{};
            const float xs[6] = {0, 128, 0, 128, 128, 0}, ys[6] = {0, 0, 96, 0, 96, 96};
            for (int i = 0; i < 6; ++i) q[i] = {xs[i], ys[i], 1.f, xs[i] / 128.f + du, ys[i] / 96.f, 0x80, 0x80, 0x80, 0x80, 0.f};
            return q;
        };
        CHECK(ps2e_draw_submit(d, &copyState, whole(0.f).data(), 6) < 0, "a frame-copy draw before any copy is refused");
        CHECK(ps2e_draw_copy_frame(d) == 1, "frame copied");
        ps2e_draw_vertex t2[3] = {{64.f, 48.f, 1.f, 0, 0, 0, 255, 0, 0x80, 0.f}, {128.f, 48.f, 1.f, 0, 0, 0, 255, 0, 0x80, 0.f},
                                  {64.f, 96.f, 1.f, 0, 0, 0, 255, 0, 0x80, 0.f}};   // green, drawn AFTER the copy
        ps2e_draw_submit(d, &flat, t2, 3);
        CHECK(ps2e_draw_submit(d, &copyState, whole(0.f).data(), 6) == 2, "identity frame-copy quad drawn");
        ps2e_draw_present(d);
        CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "frame-copy image read");
        CHECK(px(8, 8)[0] > 200 && px(8, 8)[1] < 40, "identity: the red triangle is where it was (the copy's orientation is the frame's)");
        CHECK(px(100, 80)[1] < 40 && px(100, 80)[0] < 40, "the green triangle drawn after the copy is not in it (overdrawn black)");
        CHECK(px(100, 8)[0] < 40 && px(100, 8)[1] < 40, "identity: black where nothing was drawn");
        ps2e_draw_begin_frame(d, &black);
        ps2e_draw_submit(d, &flat, t, 3);
        ps2e_draw_copy_frame(d);
        CHECK(ps2e_draw_submit(d, &copyState, whole(0.5f).data(), 6) == 2, "shifted frame-copy quad drawn");
        ps2e_draw_present(d);
        ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h);
        CHECK(px(8, 8)[0] < 40, "shifted by half a frame: the top-left is no longer red (it samples the empty right half)");
        CHECK(px(100, 8)[0] < 40 && px(72, 8)[0] < 40, "the triangle is not at u + 0.5 either (CLAMP: the copy's right edge is black)");
        // the shift the other way: u - 0.5 samples the triangle into the right half
        ps2e_draw_begin_frame(d, &black);
        ps2e_draw_submit(d, &flat, t, 3);
        ps2e_draw_copy_frame(d);
        ps2e_draw_submit(d, &copyState, whole(-0.5f).data(), 6);
        ps2e_draw_present(d);
        ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h);
        CHECK(px(72, 8)[0] > 200 && px(72, 8)[1] < 40, "u - 0.5: the triangle appears half a frame right");
        CHECK(px(8, 8)[0] > 200, "u - 0.5: the left edge clamps to the triangle's own column 0 (red)");
    }
#endif

#if defined(PS2E_DRAW_HAS_VERTEX_Q)
    // The vertex's q (the GS's STQ): S, T and Q interpolated linearly over the screen and divided per pixel. A quad over
    // x 0..128 whose left vertices carry (S 0, Q 1) and right vertices (S 4, Q 4): u' = S / Q = 4t / (1 + 3t) reaches the
    // checker's texel boundary (0.5) at t = 0.2 -- x 25.6 -- where the affine mapping (q left 0 = 1) puts it at x 64. Row
    // v fixed in the red / green texel row (T = 0.25 Q).
    {
        ps2e_draw_clear black{0, 0, 0, 1, 1};
        auto Q = [](float x, float y, float s, float t, float q) {
            ps2e_draw_vertex o{};
            o.x = x; o.y = y; o.z = 1.f; o.u = s; o.v = t; o.r = o.g = o.b = o.a = 0x80; o.q = q;
            return o;
        };
        ps2e_draw_vertex persp[6] = {Q(0, 0, 0, 0.25f, 1), Q(128, 0, 4, 1.f, 4), Q(0, 96, 0, 0.25f, 1),
                                     Q(128, 0, 4, 1.f, 4), Q(128, 96, 4, 1.f, 4), Q(0, 96, 0, 0.25f, 1)};
        ps2e_draw_begin_frame(d, &black);
        CHECK(ps2e_draw_submit(d, &textured, persp, 6) > 0, "STQ quad drawn");
        ps2e_draw_present(d);
        CHECK(ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h) == 1, "STQ image read");
        CHECK(px(16, 48)[0] > 200 && px(16, 48)[1] < 40, "STQ: x 16 (u' 0.33) is the red texel");
        CHECK(px(40, 48)[1] > 200 && px(40, 48)[0] < 40, "STQ: x 40 (u' 0.68) is the GREEN texel -- perspective-correct, not the affine midpoint");
        CHECK(px(100, 48)[1] > 200 && px(100, 48)[0] < 40, "STQ: x 100 is green");
        // the same vertices with q 0 (an older producer): the affine mapping of u = S, the boundary at x 64 of u 0..4 CLAMPed -- x 16 is u 0.5+: green
        ps2e_draw_vertex affine[6];
        std::memcpy(affine, persp, sizeof affine);
        for (auto& v : affine) v.q = 0.f;
        ps2e_draw_begin_frame(d, &black);
        ps2e_draw_submit(d, &textured, affine, 6);
        ps2e_draw_present(d);
        ps2e_draw_read_presented(d, img.data(), img.size(), &w, &h);
        CHECK(px(8, 48)[0] > 200 && px(8, 48)[1] < 40, "q 0: u = S affine, x 8 (u 0.25) red");
        CHECK(px(24, 48)[1] > 200 && px(24, 48)[0] < 40, "q 0: x 24 (u 0.75) green");
    }
#endif

    ps2e_draw_texture_destroy(d, tex);
    ps2e_draw_counts(d, &c);
    CHECK(c.textures_alive == 0, "texture destroyed");
    ps2e_draw_destroy(d);
    std::printf("test_ps2e_draw_smoke: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
