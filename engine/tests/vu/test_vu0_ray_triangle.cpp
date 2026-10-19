// Vu0RayTriangle: the retail VU0 ray/triangle microprogram (standard set, entry 0xAB8: SetRay vf4/vf5, StartRayTriangle
// vf1-vf3 + the nearest vf6, FinishRayTriangle reads VI2 == 0x900 and vf31) run on the interpreter over the retail micro
// image (a private fixture: tests/vu/fixtures/vu0_standard_code.bin, the standard set's two MPG payloads laid out at
// their micro addresses), and title::RayMath -- the value transcription a decomp's platform arm calls instead -- pinned
// to it bit for bit over hand cases and randomised rays.
//
//   test_vu0_ray_triangle [ROUNDS]   the pins (exit 0 / 1; 77 when the fixture is absent); ROUNDS randomised casts
//                                    (default 20000; a long run: test_vu0_ray_triangle 300000)
//   test_vu0_ray_triangle --probe    prints the micro's answers for the hand cases (exploration)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <random>
#include <string>

#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/hle/math/ray_math.h"
#include "tests/lib/private_fixtures.h"

static int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);       \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

namespace {

using Vector = title::RayMath::Vector;

// The unit the retail micro runs on (a record; Run below is one StartRayTriangle + FinishRayTriangle)
struct Micro {
    VuUnit vu{0};
    VuInterpreterTier tier;
};
void Load(Micro& m, const uint8_t* code) { m.vu.WriteMicroMem(0, code, 4096); }
// Returns VI2 and leaves vf31 in `hit`
uint32_t Run(Micro& m, const Vector& start, const Vector& end, const Vector t[3], const Vector& nearest, Vector& hit) {
    VUregs& r = m.vu.regs();
    std::memset(&r, 0, sizeof(r));
    r.Reset();
    auto set = [&](int reg, const Vector& v) {
        for (int k = 0; k < 4; ++k) std::memcpy(&r.VF[reg].I[k], &v[k], 4);
    };
    set(4, start);
    set(5, end);
    set(1, t[0]);
    set(2, t[1]);
    set(3, t[2]);
    set(6, nearest);
    m.tier.Run(m.vu, 0xAB8);
    for (int k = 0; k < 4; ++k) std::memcpy(&hit[k], &r.VF[31].I[k], 4);
    return r.VI[2];
}

uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

void Print(const char* label, uint32_t vi2, const Vector& hit) {
    std::printf("%-44s vi2 %#x  vf31 (%g %g %g | w %g) [%08x %08x %08x %08x]\n", label, vi2, hit[0], hit[1], hit[2], hit[3],
                Bits(hit[0]), Bits(hit[1]), Bits(hit[2]), Bits(hit[3]));
}

}  // namespace

int main(int argc, char** argv) {
    const bool probe = argc > 1 && std::string(argv[1]) == "--probe";
    const std::string path = PrivateFixtures::Path("tests/vu/fixtures/vu0_standard_code.bin");
    uint8_t code[4096];
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f || std::fread(code, 1, sizeof(code), f) != sizeof(code)) {
        if (f) std::fclose(f);
        return PrivateFixtures::Skip(path);
    }
    std::fclose(f);
    Micro micro;
    Load(micro, code);

    const float inf = INFINITY;
    const Vector none{0, 0, 0, inf};
    // A triangle in the plane z = 0 around the origin, counter-clockwise seen from -z
    const Vector tri[3] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {0, 1, 0, 1}};
    struct Case {
        const char* label;
        Vector start, end;
        Vector nearest;
    } cases[] = {
        {"through the middle, -z to +z", {0, 0, -1, 1}, {0, 0, 1, 1}, none},
        {"through the middle, +z to -z (other side)", {0, 0, 1, 1}, {0, 0, -1, 1}, none},
        {"through the middle, nearest already 0.3", {0, 0, -1, 1}, {0, 0, 1, 1}, {0, 0, 0, 0.3f}},
        {"through the middle, nearest 0.7", {0, 0, -1, 1}, {0, 0, 1, 1}, {0, 0, 0, 0.7f}},
        {"misses beside the triangle", {3, 0, -1, 1}, {3, 0, 1, 1}, none},
        {"stops short of the plane", {0, 0, -2, 1}, {0, 0, -1, 1}, none},
        {"starts past the plane", {0, 0, 1, 1}, {0, 0, 2, 1}, none},
        {"long ray, hit at share 0.1", {0, 0, -1, 1}, {0, 0, 9, 1}, none},
        {"grazing an edge (x = 0.999 at y = -1)", {0.999f, -1, -1, 1}, {0.999f, -1, 1, 1}, none},
        {"just outside an edge (y = -1.001)", {0, -1.001f, -1, 1}, {0, -1.001f, 1, 1}, none},
        {"parallel to the plane", {-2, 0, 0, 1}, {2, 0, 0, 1}, none},
        {"a camera-like cast (8 units)", {-59.0f, 4.6f, 148.8f, 1}, {-56.2f, 1.1f, 157.1f, 1}, none},
    };
    if (argc > 1 && std::string(argv[1]) == "--trace") {
        // The register file after every bundle of one cast: the program re-run truncated by an E bit on bundle k-1 (bundle k
        // runs as the delay slot), the deltas against the previous truncation printed with the bundle's words.
        const Vector s{-59.0f, 4.6f, 148.8f, 1}, e{-56.2f, 1.1f, 157.1f, 1};
        const Vector wall[3] = {{-58, 0, 150, 1}, {-58, 10, 150, 1}, {-56, 0, 160, 1}};
        const Vector nearest = argc > 2 ? Vector{0, 0, 0, float(std::atof(argv[2]))} : none;
        VUregs prev{};
        bool have_prev = false;
        // Re-read the image for the truncations
        uint8_t image[4096];
        FILE* f2 = std::fopen(path.c_str(), "rb");
        if (!f2 || std::fread(image, 1, sizeof(image), f2) != sizeof(image)) return 2;
        std::fclose(f2);
        for (int k = 1; k <= 90; ++k) {
            uint8_t code[4096];
            std::memcpy(code, image, sizeof(code));
            const uint32_t pc = 0xAB8 + uint32_t(k - 1) * 8;
            // E bit = bit 30 of the upper word (bytes 4..7 of the bundle)
            code[pc + 7] |= 0x40;
            Micro m;
            Load(m, code);
            Vector hit{};
            const uint32_t vi2 = Run(m, s, e, wall, nearest, hit);
            const VUregs& r = m.vu.regs();
            uint32_t lo, hi;
            std::memcpy(&lo, image + pc + 8, 4);
            std::memcpy(&hi, image + pc + 12, 4);
            std::printf("after bundle %2d (pc %#x, words %08x %08x) vi1 %#x vi2 %#x Q %g [%08x]", k, pc + 8, hi, lo, r.VI[1], vi2, r.Q, Bits(r.Q));
            if (have_prev) {
                for (int reg = 1; reg < 32; ++reg)
                    if (std::memcmp(&r.VF[reg], &prev.VF[reg], 16) != 0)
                        std::printf("\n    vf%-2d = (%g %g %g %g) [%08x %08x %08x %08x]", reg, r.VF[reg].F[0], r.VF[reg].F[1], r.VF[reg].F[2], r.VF[reg].F[3],
                                    r.VF[reg].I[0], r.VF[reg].I[1], r.VF[reg].I[2], r.VF[reg].I[3]);
                if (std::memcmp(&r.ACC, &prev.ACC, 16) != 0)
                    std::printf("\n    ACC  = (%g %g %g %g)", r.ACC.F[0], r.ACC.F[1], r.ACC.F[2], r.ACC.F[3]);
            }
            std::printf("\n");
            prev = r;
            have_prev = true;
            if (hi & 0x40000000u) break;   // the real E bit reached
        }
        return 0;
    }
    if (probe) {
        for (const Case& c : cases) {
            Vector hit{};
            const uint32_t vi2 = Run(micro, c.start, c.end, tri, c.nearest, hit);
            Print(c.label, vi2, hit);
        }
        // how far outside an edge still hits (the margin), for a unit-scale and a 10x triangle
        for (float scale : {1.f, 10.f, 0.1f}) {
            const Vector big[3] = {{-scale, -scale, 0, 1}, {scale, -scale, 0, 1}, {0, scale, 0, 1}};
            for (float out : {0.0001f, 0.001f, 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.f}) {
                Vector hit{};
                const float y = -scale - out;
                const uint32_t vi2 = Run(micro, {0, y, -1, 1}, {0, y, 1, 1}, big, none, hit);
                std::printf("scale %g outside edge by %g: vi2 %#x%s\n", scale, out, vi2, vi2 == 0x900 ? "  HIT" : "");
            }
            for (float out : {0.001f, 0.01f, 0.05f, 0.1f, 0.5f}) {
                Vector hit{};
                const uint32_t vi2 = Run(micro, {0, scale + out, -1, 1}, {0, scale + out, 1, 1}, big, none, hit);
                std::printf("scale %g outside the apex by %g: vi2 %#x%s\n", scale, out, vi2, vi2 == 0x900 ? "  HIT" : "");
            }
        }
        {
            const auto t0 = std::chrono::steady_clock::now();
            Vector h{};
            for (int i = 0; i < 20000; ++i) Run(micro, cases[11].start, cases[11].end, tri, none, h);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 20000;
            std::printf("interpreter: %.2f us per cast\n", us);
        }
        // the camera-like cast against a wall triangle it crosses
        const Vector wall[3] = {{-58, 0, 150, 1}, {-58, 10, 150, 1}, {-56, 0, 160, 1}};
        Vector hit{};
        const uint32_t vi2 = Run(micro, cases[11].start, cases[11].end, wall, none, hit);
        Print("camera-like cast through a wall", vi2, hit);
        return 0;
    }

    // ---- the pins: RayMath against the micro ----------------------------------------------------------------------
    size_t compared = 0, hits = 0;
    auto pin = [&](const Vector& s, const Vector& e, const Vector t[3], const Vector& nearest) {
        Vector micro_hit{};
        const uint32_t vi2 = Run(micro, s, e, t, nearest, micro_hit);
        const bool micro_nearer = vi2 == title::RayMath::kNearer;
        title::RayMath::Ray ray = title::RayMath::SetRay(s, e);
        title::RayMath::Result r = title::RayMath::Test(ray, {t[0], t[1], t[2]}, nearest);
        compared++;
        if (micro_nearer) hits++;
        if (r.nearer != micro_nearer) {
            std::printf("FAIL case %zu: micro vi2 %#x (nearer %d) RayMath nearer %d; start (%g %g %g) end (%g %g %g)\n", compared, vi2,
                        int(micro_nearer), int(r.nearer), s[0], s[1], s[2], e[0], e[1], e[2]);
            ++g_fail;
            return;
        }
        if (micro_nearer) {
            for (int k = 0; k < 4; ++k) {
                if (Bits(r.hit[k]) != Bits(micro_hit[k])) {
                    std::printf("FAIL case %zu lane %c: micro %08x (%g) RayMath %08x (%g)\n", compared, "xyzw"[k], Bits(micro_hit[k]),
                                micro_hit[k], Bits(r.hit[k]), r.hit[k]);
                    ++g_fail;
                }
            }
        }
    };
    for (const Case& c : cases) pin(c.start, c.end, tri, c.nearest);
    std::mt19937 rng(20261015);
    std::uniform_real_distribution<float> pos(-80.f, 80.f), small(-3.f, 3.f), unit(0.f, 1.f);
    // the round count comes from argv (the engine keeps a single getenv site: CoreSingleGetenvSite)
    const long rounds = (argc > 1 && std::atol(argv[1]) > 0) ? std::atol(argv[1]) : 20000;
    for (long i = 0; i < rounds && g_fail < 20; ++i) {
        // a triangle of a few units around a point, a ray of up to ~15 units that mostly crosses its plane near it
        const Vector centre{pos(rng), pos(rng) * 0.2f, pos(rng), 1};
        Vector t[3];
        for (auto& v : t) v = {centre[0] + small(rng), centre[1] + small(rng), centre[2] + small(rng), 1};
        Vector dir{small(rng), small(rng), small(rng), 0};
        const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len < 1e-3f) continue;
        const float reach = 2.f + unit(rng) * 13.f;
        for (int k = 0; k < 3; ++k) dir[k] = dir[k] / len * reach;
        const float along = unit(rng);
        const Vector target{centre[0] + small(rng) * 0.5f, centre[1] + small(rng) * 0.5f, centre[2] + small(rng) * 0.5f, 1};
        const Vector s{target[0] - dir[0] * along, target[1] - dir[1] * along, target[2] - dir[2] * along, 1};
        const Vector e{s[0] + dir[0], s[1] + dir[1], s[2] + dir[2], 1};
        const Vector nearest = (i % 4 == 0) ? Vector{0, 0, 0, unit(rng)} : none;
        pin(s, e, t, nearest);
        if (i % 7 == 0) {   // the same cast through an edge of the triangle (the margin's seam), and a degenerate triangle
            const Vector mid{(t[0][0] + t[1][0]) * 0.5f, (t[0][1] + t[1][1]) * 0.5f, (t[0][2] + t[1][2]) * 0.5f, 1};
            const Vector s2{mid[0] - dir[0] * 0.5f, mid[1] - dir[1] * 0.5f, mid[2] - dir[2] * 0.5f, 1};
            const Vector e2{s2[0] + dir[0], s2[1] + dir[1], s2[2] + dir[2], 1};
            pin(s2, e2, t, none);
            const Vector flat[3] = {t[0], t[1], t[1]};
            pin(s, e, flat, none);
        }
        if (i % 11 == 0) {   // a huge triangle (the scenery's far planes) and a tiny ray
            Vector big[3];
            for (int k = 0; k < 3; ++k) big[k] = {t[k][0] * 40.f, t[k][1] * 40.f, t[k][2] * 40.f, 1};
            pin(s, e, big, nearest);
            const Vector e3{s[0] + dir[0] * 0.01f, s[1] + dir[1] * 0.01f, s[2] + dir[2] * 0.01f, 1};
            pin(s, e3, t, none);
        }
    }
    std::printf("Vu0RayTriangle: %zu casts compared (%zu nearer hits), %d failures\n", compared, hits, g_fail);
    return g_fail ? 1 : 0;
}
