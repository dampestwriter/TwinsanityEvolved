// ScreenAspect (video.aspect): the title's screen-shape term
// answered by the session's selected ratio at the four NTSC entries, the HUD
// fits' native arithmetic, the movie picture's placement at the ratio, the
// AspectSelection snap and the presenter's picture rect. No generated body,
// GPU or game clock runs here.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "engine/ee/cpu/ps2_float.h"
#include "engine/gs/hle/present/aspect_selection.h"
#include "engine/ipu/hle/guest_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/screen_aspect.h"

namespace {
using title::ScreenAspect;
using ps2r::gs::AspectSelection;
unsigned checks = 0;
void Check(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
uint32_t Bits(float value) { uint32_t word; std::memcpy(&word, &value, 4); return word; }
float Float(uint32_t word) { float value; std::memcpy(&value, &word, 4); return value; }

struct Fixture : ps2r::ipu::GuestMemory {
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x400000);
    std::map<uint32_t, ScreenAspect::Body> bindings;
    std::map<uint32_t, unsigned> originals, interior;
    std::map<uint32_t, float> seen_f12, seen_f14;
    uint8_t* Translate(uint32_t addr, size_t len) override {
        return uint64_t(addr) + len <= ram.size() ? ram.data() + addr : nullptr;
    }
    uint32_t Read32(uint32_t addr) override {
        uint32_t v = 0; if (auto* p = Translate(addr, 4)) std::memcpy(&v, p, 4); return v;
    }
    void Write32(uint32_t addr, uint32_t value) override {
        if (auto* p = Translate(addr, 4)) std::memcpy(p, &value, 4);
    }
    void Write16(uint32_t addr, int16_t value) { std::memcpy(ram.data() + addr, &value, 2); }
    float ReadF(uint32_t addr) { return Float(Read32(addr)); }
    void WriteF(uint32_t addr, float value) { Write32(addr, Bits(value)); }
    ScreenAspect::Capture Capture() {
        return [this](uint32_t at) -> ScreenAspect::Body {
            return [this, at](CpuContext& context, uint32_t entry) {
                if (entry && entry != at) ++interior[at]; else ++originals[at];
                seen_f12[at] = context.fpuRegs.fpr[12].f;
                seen_f14[at] = context.fpuRegs.fpr[14].f;
            };
        };
    }
    ScreenAspect::Installer Install() {
        return [this](uint32_t at, ScreenAspect::Body body) { bindings[at] = std::move(body); return true; };
    }
    void Call(uint32_t at, CpuContext& c, uint32_t entry = 0) {
        Check(bindings.count(at) > 0, "address is installed");
        bindings.at(at)(c, entry);
    }
};

// The generated FitPlaceToScreen (FUN_0019ffa0) in its exact operation order,
// for the title's own two terms -- the native model must reproduce it bit for
// bit at 16:9 (the title's widescreen mode) and the test's reference at 4:3.
void GeneratedFitPlace(float aspect, bool in_pixels, float anchor_x, int16_t w, int16_t h, float& px, float& py) {
    float f1 = ps2_float_clamp(ps2_float_clamp(px) - ps2_float_clamp(anchor_x));
    float f0 = ps2_float_clamp(ps2_float_clamp(aspect) * ps2_float_clamp(0.75f));
    f1 = ps2_div_s(f1, f0);
    px = ps2_float_clamp(ps2_float_clamp(f1) + ps2_float_clamp(anchor_x));
    if (!in_pixels) return;
    px = ps2_float_clamp(ps2_float_clamp(px) * ps2_float_clamp(float(w)));
    py = ps2_float_clamp(ps2_float_clamp(py) * ps2_float_clamp(float(h)));
}

void TestTerms() {
    Check(Bits(ScreenAspect::kNarrow) == 0x3faaaaabu, "NarrowAspect is the title's 0x3faaaaab");
    Check(Bits(ScreenAspect::kWide) == 0x3fe38e39u, "WideAspect is the title's 0x3fe38e39");
    Check(Bits(ScreenAspect::Term(AspectSelection::kWide)) == 0x3fe38e39u, "16:9 term == the title's WideAspect");
    Check(Bits(ScreenAspect::Term(AspectSelection::kNarrow)) == 0x3faaaaabu, "4:3 term == the title's NarrowAspect");
    Check(ScreenAspect::kProjection == 0x0026e870 && ScreenAspect::kFrustum == 0x001eec80 &&
          ScreenAspect::kFitPlace == 0x0019ffa0 && ScreenAspect::kFitSize == 0x001a0030,
          "the four NTSC entries (pal_ntsc_map: FUN_0026f0c8/001ef410/001a0730/001a07c0)");
}

void TestSnap() {
    Check(AspectSelection::Snap(0.0) == AspectSelection::kNarrow, "no window snaps to 4:3");
    Check(AspectSelection::Snap(960.0 / 672.0) == AspectSelection::kNarrow, "960x672 (the default window) is 4:3");
    Check(AspectSelection::Snap(512.0 / 448.0) == AspectSelection::kNarrow, "512x448 is 4:3");
    Check(AspectSelection::Snap(1280.0 / 720.0) == AspectSelection::kWide, "1280x720 is 16:9");
    Check(AspectSelection::Snap(1920.0 / 1200.0) == AspectSelection::kWide16_10, "1920x1200 is 16:10");
    Check(AspectSelection::Snap(1680.0 / 1050.0) == AspectSelection::kWide16_10, "1680x1050 is 16:10");
    Check(AspectSelection::Snap(2560.0 / 1080.0) == AspectSelection::kWide21_9, "2560x1080 is 21:9");
    Check(AspectSelection::Snap(3440.0 / 1440.0) == AspectSelection::kWide21_9, "3440x1440 is 21:9");
    Check(AspectSelection::Snap(1.0) == AspectSelection::kNarrow, "a tall window is 4:3");
    Check(std::string(AspectSelection::Name(AspectSelection::kNarrow)) == "4:3" &&
          std::string(AspectSelection::Name(AspectSelection::kWide16_10)) == "16:10" &&
          std::string(AspectSelection::Name(AspectSelection::kWide)) == "16:9" &&
          std::string(AspectSelection::Name(AspectSelection::kWide21_9)) == "21:9", "names");
    AspectSelection auto_sel(AspectSelection::Shape::Auto);
    Check(auto_sel.Ratio() == AspectSelection::kNarrow, "auto without a window is 4:3");
    auto_sel.SetWindow(1920, 1080);
    Check(auto_sel.Ratio() == AspectSelection::kWide, "auto follows a 16:9 window");
    auto_sel.SetWindow(0, 0);
    Check(auto_sel.Ratio() == AspectSelection::kNarrow, "a closed window returns to 4:3");
    AspectSelection wide(AspectSelection::Shape::Wide21_9);
    wide.SetWindow(640, 480);
    Check(wide.Ratio() == AspectSelection::kWide21_9, "an explicit selection ignores the window");
    wide.SetSelection(AspectSelection::Shape::Wide16_10);
    Check(wide.Ratio() == AspectSelection::kWide16_10 && wide.Selection() == AspectSelection::Shape::Wide16_10,
          "a live change (the Display page) answers at the next read");
    wide.SetSelection(AspectSelection::Shape::Auto);
    Check(wide.Ratio() == AspectSelection::kNarrow, "a live change to auto follows the (4:3) window");
    Check(AspectSelection::Live(AspectSelection::Shape::Narrow, true), "explicit 4:3 with a window is live (a live change can leave it)");
    Check(!AspectSelection::Live(AspectSelection::Shape::Narrow, false), "explicit 4:3 headless composes nothing");
    Check(!AspectSelection::Live(AspectSelection::Shape::Auto, false), "auto without a window is not live");
    Check(AspectSelection::Live(AspectSelection::Shape::Auto, true), "auto with a window is live");
    Check(AspectSelection::Live(AspectSelection::Shape::Wide16_9, false), "explicit 16:9 is live headless");
}

void TestDispatch() {
    Fixture fixture;
    double ratio = AspectSelection::kNarrow;
    ScreenAspect model(fixture, [&] { return ratio; });
    Check(model.Install(fixture.Capture(), fixture.Install()), "install");
    Check(!model.Install(fixture.Capture(), fixture.Install()), "a second install is refused");
    CpuContext context{};
    // 4:3: every entry is the generated body, untouched (the identity).
    context.fpuRegs.fpr[12].f = 1.0f;
    fixture.Call(ScreenAspect::kProjection, context);
    Check(fixture.originals[ScreenAspect::kProjection] == 1 && fixture.seen_f12[ScreenAspect::kProjection] == 1.0f,
          "4:3 projection: the original runs with its own $f12");
    context.fpuRegs.fpr[14].f = 2.0f;
    fixture.Call(ScreenAspect::kFrustum, context);
    Check(fixture.seen_f14[ScreenAspect::kFrustum] == 2.0f, "4:3 frustum: the original runs with its own $f14");
    fixture.Call(ScreenAspect::kFitPlace, context);
    fixture.Call(ScreenAspect::kFitSize, context);
    Check(fixture.originals[ScreenAspect::kFitPlace] == 1 && fixture.originals[ScreenAspect::kFitSize] == 1,
          "4:3 fits: the originals run");
    auto counts = model.Counts();
    Check(counts.passed[0] == 1 && counts.passed[1] == 1 && counts.passed[2] == 1 && counts.passed[3] == 1 &&
          counts.owned[0] + counts.owned[1] + counts.owned[2] + counts.owned[3] == 0, "4:3 counted as passed");
    // 16:9: the projection's and the frustum's aspect arguments are the title's WideAspect.
    ratio = AspectSelection::kWide;
    context.fpuRegs.fpr[12].f = 1.0f;
    fixture.Call(ScreenAspect::kProjection, context);
    Check(fixture.originals[ScreenAspect::kProjection] == 2, "16:9 projection: the generated body runs");
    Check(Bits(fixture.seen_f12[ScreenAspect::kProjection]) == 0x3fe38e39u, "16:9 projection: $f12 == WideAspect");
    context.fpuRegs.fpr[12].f = 0.1f;
    context.fpuRegs.fpr[13].f = 1500.0f;
    context.fpuRegs.fpr[14].f = 1.3333334f;
    fixture.Call(ScreenAspect::kFrustum, context, ScreenAspect::kFrustum);
    Check(Bits(fixture.seen_f14[ScreenAspect::kFrustum]) == 0x3fe38e39u, "16:9 frustum: $f14 == WideAspect");
    Check(fixture.seen_f12[ScreenAspect::kFrustum] == 0.1f && context.fpuRegs.fpr[13].f == 1500.0f,
          "16:9 frustum: near/far untouched");
    // 21:9: the term the title never had.
    ratio = AspectSelection::kWide21_9;
    fixture.Call(ScreenAspect::kProjection, context);
    Check(fixture.seen_f12[ScreenAspect::kProjection] == float(21.0 / 9.0), "21:9 projection term");
    // An interior entry runs the original with its entry and touches nothing.
    context.fpuRegs.fpr[12].f = 7.0f;
    fixture.Call(ScreenAspect::kProjection, context, 0x26e8e0);
    Check(fixture.interior[ScreenAspect::kProjection] == 1 && fixture.seen_f12[ScreenAspect::kProjection] == 7.0f,
          "interior entry: delegated untouched");
    counts = model.Counts();
    Check(counts.owned[0] == 2 && counts.owned[1] == 1 && counts.interior == 1, "owned/interior counts");
    const std::string census = model.Census();
    Check(census.rfind("ratio=21:9,projection=2,passed_projection=1,frustum=1,passed_frustum=1,fit_place=0,"
                       "passed_fit_place=1,fit_size=0,passed_fit_size=1,interior=1", 0) == 0,
          ("census: " + census).c_str());
}

void TestFits() {
    // The native fit against the generated order at the title's own 16:9 term:
    // bit-identical (the same EE operations in the same order).
    const int16_t width = 512, height = 448;
    const float samples[] = {0.0f, 0.1f, 0.25f, 0.5f, 0.73f, 1.0f, -0.2f, 1.3f};
    for (float anchor : {0.0f, 0.5f, 1.0f})
        for (float x : samples)
            for (float y : samples)
                for (bool pixels : {false, true}) {
                    float ax = x, ay = y, bx = x, by = y;
                    ScreenAspect::FitPlace(ScreenAspect::kWide, pixels, anchor, width, height, ax, ay);
                    GeneratedFitPlace(ScreenAspect::kWide, pixels, anchor, width, height, bx, by);
                    Check(Bits(ax) == Bits(bx) && Bits(ay) == Bits(by), "FitPlace == the generated order at 16:9");
                }
    // The anchoring: a left-anchored place is squeezed towards the left edge by
    // 0.75 (pinned once the frame is stretched), a centred one stays centred, a
    // right-anchored one stays pinned to the right.
    float px = 0.1f, py = 0.9f;
    ScreenAspect::FitPlace(ScreenAspect::kWide, false, 0.0f, width, height, px, py);
    Check(std::fabs(px - 0.075f) < 1e-6f && py == 0.9f, "left anchor: x 0.1 -> 0.075, y untouched");
    px = 0.5f;
    ScreenAspect::FitPlace(ScreenAspect::kWide, false, 0.5f, width, height, px, py);
    Check(px == 0.5f, "centre anchor: the centre stays");
    px = 0.9f;
    ScreenAspect::FitPlace(ScreenAspect::kWide, false, 1.0f, width, height, px, py);
    Check(std::fabs(px - 0.925f) < 1e-6f, "right anchor: x 0.9 -> 0.925 (pinned to the right edge)");
    px = 0.3f; py = 0.5f;
    ScreenAspect::FitPlace(ScreenAspect::kWide, true, 0.0f, width, height, px, py);
    Check(std::fabs(px - 0.225f * 512) < 1e-3f && py == 224.0f, "in pixels: x and y scaled by the renderer size");
    // 21:9: the squeeze is (4/3)/(21/9) = 4/7.
    px = 0.7f;
    ScreenAspect::FitPlace(float(21.0 / 9.0), false, 0.0f, width, height, px, py);
    Check(std::fabs(px - 0.4f) < 1e-5f, "21:9 left anchor: 0.7 -> 0.4");
    // Sizes shrink by the same squeeze (no stretched HUD once the frame is stretched).
    float sx = 0.4f, sy = 0.2f;
    ScreenAspect::FitSize(ScreenAspect::kWide, false, width, height, sx, sy);
    Check(std::fabs(sx - 0.3f) < 1e-6f && sy == 0.2f, "size x 0.4 -> 0.3 at 16:9");
    sx = 0.4f; sy = 0.2f;
    ScreenAspect::FitSize(1.6f, true, width, height, sx, sy);
    Check(std::fabs(sx - (0.4f / 1.2f) * 512) < 1e-2f && std::fabs(sy - 0.2f * 448) < 1e-3f, "16:10 size in pixels");
    // At 4:3 the squeeze is 1: the fit leaves the place where the title made it.
    px = 0.3f; py = 0.4f;
    ScreenAspect::FitPlace(ScreenAspect::kNarrow, false, 0.0f, width, height, px, py);
    Check(std::fabs(px - 0.3f) < 1e-6f, "4:3: squeeze 1");
}

void TestOwnedFits() {
    // The owned entries read the title's arguments and renderer size from guest memory.
    Fixture fixture;
    double ratio = AspectSelection::kWide;
    ScreenAspect model(fixture, [&] { return ratio; });
    Check(model.Install(fixture.Capture(), fixture.Install()), "install");
    fixture.Write16(ScreenAspect::kRendererWidth, 512);
    fixture.Write16(ScreenAspect::kRendererHeight, 448);
    const uint32_t anchor = 0x1000, place = 0x1010, size = 0x1020;
    fixture.WriteF(anchor, 0.0f); fixture.WriteF(anchor + 4, 0.0f);
    fixture.WriteF(place, 0.2f); fixture.WriteF(place + 4, 0.5f);
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 1;   // inPixels
    context.cpuRegs.GPR.r[5].UL[0] = anchor;
    context.cpuRegs.GPR.r[6].UL[0] = place;
    fixture.Call(ScreenAspect::kFitPlace, context);
    Check(fixture.originals[ScreenAspect::kFitPlace] == 0, "16:9 fit place: the original does not run");
    Check(std::fabs(fixture.ReadF(place) - 0.15f * 512) < 1e-3f && fixture.ReadF(place + 4) == 224.0f,
          "16:9 fit place in pixels: (0.2, 0.5) -> (76.8, 224)");
    fixture.WriteF(place, 0.2f); fixture.WriteF(place + 4, 0.5f);
    context.cpuRegs.GPR.r[4].UL[0] = 0;
    fixture.Call(ScreenAspect::kFitPlace, context);
    Check(std::fabs(fixture.ReadF(place) - 0.15f) < 1e-6f && fixture.ReadF(place + 4) == 0.5f,
          "16:9 fit place in units: y untouched");
    fixture.WriteF(size, 0.4f); fixture.WriteF(size + 4, 0.25f);
    context.cpuRegs.GPR.r[4].UL[0] = 1;
    context.cpuRegs.GPR.r[5].UL[0] = size;
    fixture.Call(ScreenAspect::kFitSize, context);
    Check(std::fabs(fixture.ReadF(size) - 0.3f * 512) < 1e-3f && fixture.ReadF(size + 4) == 112.0f,
          "16:9 fit size in pixels: (0.4, 0.25) -> (153.6, 112)");
    Check(model.Counts().owned[2] == 2 && model.Counts().owned[3] == 1, "owned fit counts");
}

void TestMoviePlacement() {
    using View = title::MovieGraphics::View;
    const auto extent = [](const ps2r::gs::DrawIr& draw, float& x0, float& x1, float& y0, float& y1) {
        x0 = y0 = 1e9f; x1 = y1 = -1e9f;
        for (const auto& v : draw.verts) { x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y); }
    };
    ps2r::gs::DrawIr draw;
    float x0, x1, y0, y1;
    // The title's own rule (tv_aspect 0): a 4:3 movie on a 4:3 TV fills the frame.
    View narrow{512, 448, 512, 448, false, false, 0.0f};
    Check(title::MovieGraphics::BuildDraw(narrow, draw), "4:3 movie on 4:3");
    extent(draw, x0, x1, y0, y1);
    Check(x0 == 0 && x1 == 512 && y0 == 0 && y1 == 448, "fills the frame");
    // The title's 16:9 rule and the selected 16:9 ratio agree: 3/4 of the width, centred.
    View wide_flag{512, 448, 512, 448, true, false, 0.0f};
    Check(title::MovieGraphics::BuildDraw(wide_flag, draw), "4:3 movie on the title's 16:9");
    extent(draw, x0, x1, y0, y1);
    Check(x0 == 64 && x1 == 448 && y0 == 0 && y1 == 448, "the title's rule: x 64..448");
    View wide_sel{512, 448, 512, 448, false, false, float(16.0 / 9.0)};
    Check(title::MovieGraphics::BuildDraw(wide_sel, draw), "4:3 movie at the selected 16:9");
    extent(draw, x0, x1, y0, y1);
    Check(x0 == 64 && x1 == 448 && y0 == 0 && y1 == 448, "selected 16:9 == the title's rule (pillarbox 64..448)");
    // 21:9: (4/3)/(21/9) = 4/7 of the width -> 292, rounded down to 32-px strips = 288, centred.
    View ultra{512, 448, 512, 448, false, false, float(21.0 / 9.0)};
    Check(title::MovieGraphics::BuildDraw(ultra, draw), "4:3 movie at 21:9");
    extent(draw, x0, x1, y0, y1);
    Check(x0 == 112 && x1 == 400 && y0 == 0 && y1 == 448, "21:9 pillarbox: x 112..400");
    // 16:10: 5/6 of the width -> 426 -> 416, centred at 48.
    View w1610{512, 448, 512, 448, false, false, 1.6f};
    Check(title::MovieGraphics::BuildDraw(w1610, draw), "4:3 movie at 16:10");
    extent(draw, x0, x1, y0, y1);
    Check(x0 == 48 && x1 == 464 && y0 == 0 && y1 == 448, "16:10 pillarbox: x 48..464");
    // A 16:9 movie on a 16:10 TV is letterboxed: 448 * 1.6/(16/9) = 403 rows, centred.
    View wide_movie_1610{512, 448, 512, 448, false, true, 1.6f};
    Check(title::MovieGraphics::BuildDraw(wide_movie_1610, draw), "16:9 movie at 16:10");
    extent(draw, x0, x1, y0, y1);
    Check(x0 == 0 && x1 == 512 && y0 == 22 && y1 == 425, "16:10 letterbox: y 22..425");
    // A 16:9 movie at 16:9 fills the frame; at 21:9 it is pillarboxed to 16/21 of the width.
    View wide_movie_169{512, 448, 512, 448, false, true, float(16.0 / 9.0)};
    Check(title::MovieGraphics::BuildDraw(wide_movie_169, draw), "16:9 movie at 16:9");
    extent(draw, x0, x1, y0, y1);
    Check(x0 == 0 && x1 == 512 && y0 == 0 && y1 == 448, "16:9 movie at 16:9 fills the frame");
    View wide_movie_219{512, 448, 512, 448, false, true, float(21.0 / 9.0)};
    Check(title::MovieGraphics::BuildDraw(wide_movie_219, draw), "16:9 movie at 21:9");
    extent(draw, x0, x1, y0, y1);
    Check(x0 == 64 && x1 == 448 && y0 == 0 && y1 == 448, "21:9 pillarbox of a 16:9 movie: 390 -> 384 wide");
}

}  // namespace

int main() {
    TestTerms();
    TestSnap();
    TestDispatch();
    TestFits();
    TestOwnedFits();
    TestMoviePlacement();
    std::printf("ScreenAspect: %u checks passed\n", checks);
    return 0;
}
