// tests/gs/targets/test_renderer_profile.cpp -- the title's RendererProfile fills
// every VramLayout field the engine queries; an empty layout answers false everywhere.
#include <cstdio>

#include "engine/gs/hle/targets/vram_layout.h"
#include "games/slus_20909_crash_twinsanity/renderer_profile/renderer_profile.h"

using namespace ps2r;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; }  \
    } while (0)

int main() {
    gs::VramLayout e;
    CHECK(e.Empty());
    CHECK(!e.IsSceneAccum(0x230000) && !e.IsFogLane(0x280000));
    CHECK(!e.IsFmvBlitSignature(0x70000, 0x150000, 0, 10, 10));
    CHECK(!e.IsNativeScanout(0x70000, 512, 0));
    CHECK(!e.IsClutHealExempt(0x388000) && !e.IsReaderBandPage(0x161));

    title::RendererProfile p;
    const gs::VramLayout& v = p.Vram();
    CHECK(!v.Empty());
    CHECK(v.IsSceneAccum(0x230000) && !v.IsSceneAccum(0x70000));
    CHECK(v.IsFogLane(0x280000) && v.IsFogLane(0x2F0000) && !v.IsFogLane(0x230000));
    CHECK(v.IsFmvBlitSignature(0x70000, 0x150000, 0, 10, 10));
    CHECK(!v.IsFmvBlitSignature(0x70000, 0x150000, 2, 10, 10));
    CHECK(v.IsNativeScanout(0x70000, 512, 0) && !v.IsNativeScanout(0x0, 512, 0));
    CHECK(v.stencil_zcopy_fbp == 0x268000 && v.main_fb_fbp == 0x70000);
    CHECK(v.reader_band_pages.size() == 31);
    CHECK(v.IsReaderBandPage(0x161) && v.IsReaderBandPage(0x1AB) && v.IsReaderBandPage(0x1A0));
    CHECK(!v.IsReaderBandPage(0x160) && !v.IsReaderBandPage(0x164) && !v.IsReaderBandPage(0x19C));
    CHECK(v.IsClutHealExempt(0x388000) && v.IsClutHealExempt(0x3A8000) && v.IsClutHealExempt(0x3F8000));
    CHECK(!v.IsClutHealExempt(0x380C00) && !v.IsClutHealExempt(0x375400) && !v.IsClutHealExempt(0x408000));
    std::printf("test_renderer_profile: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
