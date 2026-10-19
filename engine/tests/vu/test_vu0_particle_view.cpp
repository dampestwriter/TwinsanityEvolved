// Vu0ParticleView: the retail culling VU0 set's particle block view (entries 0xA08 / 0xB90: TR ParticleBlockView -- the
// extents in vf2, the gravity rows in vf3..vf5, the place in vf6, the scale in vf7.x, the view's VU0 address in vi1; the
// matrix left in vf29..vf26, the distance in vf30.x, the clipping flags in vi2) run on the interpreter over the retail micro
// image (a private fixture: tests/vu/fixtures/vu0_culling_code.bin, the culling set's MPG payload at its micro addresses),
// and title::ParticleViewMath -- the value transcription a decomp's platform arm calls instead -- pinned to it bit for bit
// over hand cases and randomised blocks. SKIPs without the fixture.
//
//   test_vu0_particle_view            the pins
//   test_vu0_particle_view --probe    prints the micro's answers for the hand cases (exploration)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/hle/math/particle_view_math.h"
#include "tests/lib/private_fixtures.h"

#define EXPECT(cond, ...)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
            return 1;                                                       \
        }                                                                   \
    } while (0)

namespace {

using Vector = title::ParticleViewMath::Vector;
using Matrix = title::ParticleViewMath::Matrix;
using View = title::ParticleViewMath::View;

constexpr uint32_t kViewAddress = 0x40;   // quadwords: the view record's VU0 data address for the run

struct Micro {
    VuUnit vu{0};
    VuInterpreterTier tier;
};
void Load(Micro& m, const uint8_t* code) { m.vu.WriteMicroMem(0, code, 4096); }

struct Answer {
    Matrix matrix;
    float distance;
    uint32_t vi2;
};
Answer Run(Micro& m, const View& view, const Vector& extents, const Matrix& gravity, const Vector& place, float scale, bool keeps) {
    m.vu.WriteDataMem(kViewAddress * 16, reinterpret_cast<const uint8_t*>(view.qw.data()), view.qw.size() * 16);
    VUregs& r = m.vu.regs();
    std::memset(&r, 0, sizeof(r));
    r.Reset();
    auto set = [&](int reg, const Vector& v) {
        for (int k = 0; k < 4; ++k) std::memcpy(&r.VF[reg].I[k], &v[k], 4);
    };
    set(2, extents);
    set(3, gravity[0]);
    set(4, gravity[1]);
    set(5, gravity[2]);
    set(6, place);
    set(7, {scale, 0.f, 0.f, 1.f});
    r.VI[1] = kViewAddress;
    m.tier.Run(m.vu, keeps ? 0xB90 : 0xA08);
    Answer a{};
    for (int row = 0; row < 4; ++row)
        for (int k = 0; k < 4; ++k) std::memcpy(&a.matrix[row][k], &r.VF[29 - row].I[k], 4);
    std::memcpy(&a.distance, &r.VF[30].I[0], 4);
    a.vi2 = r.VI[2];
    return a;
}

uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
bool Same(float a, float b) { return Bits(a) == Bits(b); }
bool Same(const Vector& a, const Vector& b) { return Same(a[0], b[0]) && Same(a[1], b[1]) && Same(a[2], b[2]) && Same(a[3], b[3]); }

void Print(const char* label, const Answer& a) {
    std::printf("%-40s vi2 %#x distance %g\n", label, a.vi2, a.distance);
    for (int row = 0; row < 4; ++row)
        std::printf("    row %d (%g %g %g %g)\n", row, a.matrix[row][0], a.matrix[row][1], a.matrix[row][2], a.matrix[row][3]);
}

// A view record: six frustum planes (the first group's four lanes, the second's two), the camera and a perspective-ish
// matrix to the screen
View MakeView(const Vector& camera, const Matrix& toScreen, const Vector planes[6]) {
    View v{};
    for (int g = 0; g < 2; ++g) {
        const int base = g * 7;
        for (int lane = 0; lane < 4; ++lane) {
            const int pi = g * 4 + lane;
            const Vector plane = pi < 6 ? planes[pi] : Vector{0, 0, 0, 0};
            for (int c = 0; c < 3; ++c) {
                v.qw[base + c][lane] = std::fabs(plane[c]);
                v.qw[base + 3 + c][lane] = plane[c];
            }
            v.qw[base + 6][lane] = plane[3];
        }
    }
    v.qw[14] = camera;
    for (int r = 0; r < 4; ++r) v.qw[15 + r] = toScreen[r];
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    const bool probe = argc > 1 && std::string(argv[1]) == "--probe";
    const std::string path = PrivateFixtures::Path("tests/vu/fixtures/vu0_culling_code.bin");
    uint8_t code[4096];
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f || std::fread(code, 1, sizeof(code), f) != sizeof(code)) {
        if (f) std::fclose(f);
        return PrivateFixtures::Skip(path);
    }
    std::fclose(f);
    Micro micro;
    Load(micro, code);

    // A camera at (1, 2, 3) looking down +z; the screen matrix a scaled perspective (w = the camera depth)
    const Vector camera{1.f, 2.f, 3.f, 1.f};
    const Matrix toScreen{{{1.811f * 256.f, 0.f, 0.f, 0.f},
                           {0.f, -1.811f * 256.f, 0.f, 0.f},
                           {0.f, 0.f, -98.2f, 1.f},
                           {2048.f * 16.f - 1.811f * 256.f, 2048.f * 16.f + 2.f * 1.811f * 256.f, 1677840.f - 3.f * -98.2f, -3.f}}};
    const Vector planes[6] = {{0, 0, 1, -3.5f}, {0, 0, -1, 200.f}, {0.7f, 0, 0.7f, 0}, {-0.7f, 0, 0.7f, 0}, {0, 0.7f, 0.7f, 0}, {0, -0.7f, 0.7f, 0}};
    const View view = MakeView(camera, toScreen, planes);
    const Matrix gravity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {5, 6, 7, 1}}};
    const Matrix tilted{{{0.8f, 0.6f, 0, 0}, {-0.6f, 0.8f, 0, 0}, {0, 0, 1, 0}, {5, 6, 7, 1}}};
    const Vector extents{0.5f, 0.5f, 0.5f, 1.f};

    struct Case {
        const char* label;
        Vector place;
        float scale;
        bool keeps;
        const Matrix* gravity;
    } cases[] = {
        {"ahead, scale 0", {1.f, 2.f, 13.f, 1.f}, 0.f, false, &gravity},
        {"ahead, scale 2.1 (pulled 2.1 units)", {1.f, 2.f, 13.f, 1.f}, 2.1f, false, &gravity},
        {"ahead, tilted gravity, scale 0.5", {3.f, 1.f, 20.f, 1.f}, 0.5f, false, &tilted},
        {"behind the camera (past the near plane)", {1.f, 2.f, -5.f, 1.f}, 0.f, false, &gravity},
        {"far past the far plane", {1.f, 2.f, 500.f, 1.f}, 0.f, false, &gravity},
        {"beside (past the right plane)", {60.f, 2.f, 10.f, 1.f}, 0.f, false, &gravity},
        {"keepsTranslation, scale 0", {4.f, 0.f, 9.f, 1.f}, 0.f, true, &tilted},
        {"keepsTranslation, scale 1.5", {4.f, 0.f, 9.f, 1.f}, 1.5f, true, &tilted},
        {"at the camera (zero distance)", {1.f, 2.f, 3.f, 1.f}, 1.f, false, &gravity},
    };
    if (probe) {
        for (const auto& c : cases) {
            Print(c.label, Run(micro, view, extents, *c.gravity, c.place, c.scale, c.keeps));
            const auto t = title::ParticleViewMath::BlockView(view, extents, *c.gravity, c.place, c.scale, c.keeps);
            Print("  transcription", Answer{t.matrix, t.distance, t.flags});
        }
        return 0;
    }

    // ---- the pins: ParticleViewMath against the micro -------------------------------------------------------------
    auto pin = [&](const char* label, const Vector& place, float scale, bool keeps, const Matrix& g) -> int {
        const Answer a = Run(micro, view, extents, g, place, scale, keeps);
        const auto t = title::ParticleViewMath::BlockView(view, extents, g, place, scale, keeps);
        EXPECT(a.vi2 == t.flags, "%s: vi2 %#x vs flags %#x", label, a.vi2, t.flags);
        EXPECT(Same(a.distance, t.distance), "%s: distance %g (%08x) vs %g (%08x)", label, a.distance, Bits(a.distance), t.distance, Bits(t.distance));
        for (int row = 0; row < 4; ++row)
            EXPECT(Same(a.matrix[row], t.matrix[row]), "%s: row %d micro (%g %g %g %g) [%08x %08x %08x %08x] vs (%g %g %g %g) [%08x %08x %08x %08x]", label, row,
                   a.matrix[row][0], a.matrix[row][1], a.matrix[row][2], a.matrix[row][3], Bits(a.matrix[row][0]), Bits(a.matrix[row][1]),
                   Bits(a.matrix[row][2]), Bits(a.matrix[row][3]), t.matrix[row][0], t.matrix[row][1], t.matrix[row][2], t.matrix[row][3],
                   Bits(t.matrix[row][0]), Bits(t.matrix[row][1]), Bits(t.matrix[row][2]), Bits(t.matrix[row][3]));
        return 0;
    };
    for (const auto& c : cases)
        if (pin(c.label, c.place, c.scale, c.keeps, *c.gravity)) return 1;

    // The semantics the port relies on: scale 0 leaves the place where it is (the provisional lerp put it AT the camera)
    {
        const auto t = title::ParticleViewMath::BlockView(view, extents, gravity, {1.f, 2.f, 13.f, 1.f}, 0.f, false);
        EXPECT(std::fabs(t.matrix[3][3] - 10.f) < 1e-3f, "scale 0: row 3 w %g, want the place's depth 10", t.matrix[3][3]);
        const auto u = title::ParticleViewMath::BlockView(view, extents, gravity, {1.f, 2.f, 13.f, 1.f}, 2.f, false);
        EXPECT(std::fabs(u.matrix[3][3] - 8.f) < 1e-3f, "scale 2: row 3 w %g, want the place pulled 2 units to depth 8", u.matrix[3][3]);
        EXPECT(t.flags == 0, "ahead: flags %#x", t.flags);
        const auto o = title::ParticleViewMath::BlockView(view, extents, gravity, {1.f, 2.f, -5.f, 1.f}, 0.f, false);
        EXPECT(o.flags != 0, "behind the camera: flags 0");
    }

    // Randomised blocks: places around the camera, scales 0..3, both programs, two gravities
    std::mt19937 rng(20261017);
    std::uniform_real_distribution<float> pos(-60.f, 60.f), depth(-10.f, 250.f), sc(0.f, 3.f), ex(0.1f, 8.f);
    int checked = 0;
    for (int i = 0; i < 20000; ++i) {
        const Vector place{pos(rng), pos(rng), depth(rng), 1.f};
        const Vector e{ex(rng), ex(rng), ex(rng), 1.f};
        const float scale = (i % 7 == 0) ? 0.f : sc(rng);
        const bool keeps = (i % 3) == 0;
        const Matrix& g = (i % 2) ? gravity : tilted;
        const Answer a = Run(micro, view, e, g, place, scale, keeps);
        const auto t = title::ParticleViewMath::BlockView(view, e, g, place, scale, keeps);
        char label[160];
        std::snprintf(label, sizeof label, "random %d place (%g %g %g) scale %g keeps %d", i, place[0], place[1], place[2], scale, int(keeps));
        EXPECT(a.vi2 == t.flags, "%s: vi2 %#x vs flags %#x", label, a.vi2, t.flags);
        EXPECT(Same(a.distance, t.distance), "%s: distance %08x vs %08x", label, Bits(a.distance), Bits(t.distance));
        for (int row = 0; row < 4; ++row)
            EXPECT(Same(a.matrix[row], t.matrix[row]), "%s: row %d [%08x %08x %08x %08x] vs [%08x %08x %08x %08x]", label, row,
                   Bits(a.matrix[row][0]), Bits(a.matrix[row][1]), Bits(a.matrix[row][2]), Bits(a.matrix[row][3]),
                   Bits(t.matrix[row][0]), Bits(t.matrix[row][1]), Bits(t.matrix[row][2]), Bits(t.matrix[row][3]));
        checked++;
    }
    std::printf("PASS Vu0ParticleView: %zu hand cases + %d randomised blocks pinned to the 0xA08 / 0xB90 micro\n", sizeof(cases) / sizeof(cases[0]), checked);
    return 0;
}
