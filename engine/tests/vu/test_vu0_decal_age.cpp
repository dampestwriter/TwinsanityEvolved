// Vu0DecalAge: the retail decal set's AgeDecal microprograms (entries 0x1C0, a decal in the camera's chunk, and 0x360,
// one placed in another chunk) run on the interpreter over the retail micro image (a private fixture:
// tests/vu/fixtures/vu0_decal_code.bin, the decal set's MPG payloads laid out at their micro addresses), and
// title::DecalMath -- the value transcription the decomp's platform arm calls instead -- pinned to it bit for bit over hand
// cases and randomised decals, views and variants.
//
//   test_vu0_decal_age [ROUNDS]   the pins (exit 0 / 1; 77 when the fixture is absent); ROUNDS randomised decals per
//                                 program (default 20000)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/hle/math/decal_math.h"
#include "tests/lib/private_fixtures.h"

namespace {

using title::DecalMath;
using Vector = DecalMath::Vector;

struct Micro {
    VuUnit vu{0};
    VuInterpreterTier tier;
};

DecalMath::Look RunMicro(Micro& m, const std::vector<Vector>& data, uint32_t program, const DecalMath::Decal& d) {
    m.vu.WriteDataMem(0, reinterpret_cast<const uint8_t*>(data.data()), data.size() * 16);
    VUregs& r = m.vu.regs();
    r.Reset();
    for (int k = 0; k < 4; ++k) {
        std::memcpy(&r.VF[1].I[k], &d.place[k], 4);
        r.VF[2].I[k] = uint32_t(d.frame[k]);
        r.VF[3].I[k] = uint32_t(d.more[k]);
    }
    r.VI[1] = d.variant;
    m.tier.Run(m.vu, program);
    DecalMath::Look look;
    for (int k = 0; k < 4; ++k) {
        look.place[k] = r.VF[28].I[k];
        look.colour[k] = r.VF[29].I[k];
        look.offsets[k] = r.VF[31].I[k];
        look.more[k] = r.VF[30].I[k];
    }
    return look;
}

int g_fail = 0;
int g_printed = 0;
long g_outside = 0, g_inside = 0;

bool Same(const DecalMath::Look& a, const DecalMath::Look& b) {
    return a.place == b.place && a.colour == b.colour && a.offsets == b.offsets && a.more == b.more;
}
void Report(const char* what, uint32_t program, const DecalMath::Decal& d, const DecalMath::Look& micro, const DecalMath::Look& port) {
    ++g_fail;
    if (g_printed++ >= 12) return;
    auto row = [](const char* name, const std::array<uint32_t, 4>& m, const std::array<uint32_t, 4>& p) {
        std::printf("    %-8s micro %08x %08x %08x %08x | port %08x %08x %08x %08x%s\n", name, m[0], m[1], m[2], m[3], p[0], p[1], p[2],
                    p[3], m == p ? "" : "   <--");
    };
    std::printf("FAIL %s program %#x place (%g %g %g life %g) variant %#x\n", what, program, d.place[0], d.place[1], d.place[2], d.place[3],
                d.variant);
    row("place", micro.place, port.place);
    row("colour", micro.colour, port.colour);
    row("offsets", micro.offsets, port.offsets);
    row("more", micro.more, port.more);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = PrivateFixtures::Path("tests/vu/fixtures/vu0_decal_code.bin");
    uint8_t code[4096];
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f || std::fread(code, 1, sizeof(code), f) != sizeof(code)) {
        if (f) std::fclose(f);
        return PrivateFixtures::Skip(path);
    }
    std::fclose(f);
    const long rounds = argc > 1 ? std::atol(argv[1]) : 20000;
    Micro micro;
    micro.vu.WriteMicroMem(0, code, sizeof(code));

    std::mt19937 rng(0x5EED);
    auto uni = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    auto plane = [&]() {
        Vector n{uni(-1, 1), uni(-1, 1), uni(-1, 1), uni(-200, 200)};
        return n;
    };
    // A view as LoadDecalView leaves it: four side planes transposed, the near plane four times transposed, a chunk matrix
    // (rotation-ish rows and a translation), each variant's colour keys and size keys (qw 5's w the life rate)
    auto makeData = [&]() {
        std::vector<Vector> data(DecalMath::kDataQuadwords);
        Vector sides[4] = {plane(), plane(), plane(), plane()};
        for (int lane = 0; lane < 4; ++lane)
            for (int c = 0; c < 4; ++c) data[c][lane] = sides[lane][c];
        const Vector nearPlane = plane();
        for (int c = 0; c < 4; ++c) data[4 + c] = {nearPlane[c], nearPlane[c], nearPlane[c], nearPlane[c]};
        for (int r = 0; r < 3; ++r) data[8 + r] = {uni(-1, 1), uni(-1, 1), uni(-1, 1), 0.f};
        data[11] = {uni(-500, 500), uni(-50, 50), uni(-500, 500), 1.f};
        for (uint32_t q = 0x10; q < 0x10 + 15 * 8; ++q) {
            const uint32_t key = (q - 0x10) % 8;
            if (key < 4) data[q] = {uni(0, 255), uni(0, 255), uni(0, 255), uni(0, 128)};
            else data[q] = {uni(0.05f, 4), uni(0, 4), uni(0.05f, 4), uni(0.1f, 3)};
        }
        return data;
    };
    auto makeDecal = [&](bool wild) {
        DecalMath::Decal d;
        d.place = {uni(-400, 400), uni(-40, 40), uni(-400, 400), wild ? uni(-2, 6) : uni(0, 3)};
        for (int k = 0; k < 4; ++k) {
            d.frame[k] = int32_t(std::uniform_int_distribution<int>(-32768, 32767)(rng));
            d.more[k] = int32_t(std::uniform_int_distribution<int>(-32768, 32767)(rng));
        }
        d.variant = uint16_t(0x10 + 8 * std::uniform_int_distribution<int>(0, 14)(rng));
        return d;
    };

    long checked = 0;
    for (uint32_t program : {DecalMath::kSameChunk, DecalMath::kOtherChunk}) {
        // hand cases: a fresh decal at the origin, one at its end of life, one dead past it
        {
            std::vector<Vector> data = makeData();
            for (float life : {0.f, 0.5f, 1.f, 2.5f, 3.f, -1.f}) {
                DecalMath::Decal d;
                d.place = {0.f, 0.f, 0.f, life};
                d.frame = {16384, -16384, 0, 32767};
                d.more = {-16384, 16384, 32767, 0};
                d.variant = 0x10;
                const auto m = RunMicro(micro, data, program, d), p = DecalMath::Age(data.data(), program, d);
                ++checked;
                if (!Same(m, p)) Report("hand", program, d, m, p);
            }
        }
        std::vector<Vector> data = makeData();
        for (long i = 0; i < rounds; ++i) {
            if (i % 64 == 0) data = makeData();
            const DecalMath::Decal d = makeDecal(i % 4 == 0);
            const auto m = RunMicro(micro, data, program, d), p = DecalMath::Age(data.data(), program, d);
            ++checked;
            (m.offsets[3] & 0xFFFF ? g_outside : g_inside)++;
            if (!Same(m, p)) Report("random", program, d, m, p);
        }
    }
    std::printf("%s: %ld decals checked (random: %ld outside the view, %ld inside), %d differing\n", g_fail ? "FAIL" : "PASS", checked,
                g_outside, g_inside, g_fail);
    return g_fail ? 1 : 0;
}
