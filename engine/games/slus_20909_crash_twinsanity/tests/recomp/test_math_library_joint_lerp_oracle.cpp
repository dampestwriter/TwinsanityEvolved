#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_lerp_oracle.h"
#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"

#include <cfenv>
#include <cstdio>
#include <cstring>

namespace {
using Math = title::JointMath;
unsigned checks = 0, failures = 0;
uint64_t cases = 0;
void Check(bool good, const char* message) {
    ++checks;
    if (!good && failures++ < 16) std::fprintf(stderr, "%s\n", message);
}
uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
void Input(Math::Vector& vector, uint32_t& seed, unsigned trial) {
    constexpr std::array<uint32_t, 12> pins{
        0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
        0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fffffff, 0xffffffff};
    for (auto& lane : vector) {
        const auto word = Next(seed);
        lane = trial % 5 == 0 ? Float(pins[word % pins.size()]) :
            trial % 5 == 1 ? Float(word) : static_cast<float>(static_cast<int32_t>(word)) / 1073741824.0f;
    }
}
bool Blend(unsigned trial, uint32_t& seed, int mode) {
    Math::Vector now, next, carrier;
    Input(now, seed, trial); Input(next, seed, trial); Input(carrier, seed, trial);
    if (trial % 7 == 0) now = {1, 1, 1, Float(Next(seed))};
    if (trial % 11 == 0) next = now;
    if (trial % 3 == 0) carrier[3] = static_cast<float>(Next(seed) % 1025) / 1024.0f;
    CpuContext control{};
    for (auto& word : reinterpret_cast<uint32_t(&)[sizeof(control) / 4]>(control)) word = Next(seed);
    control.vuRegs.VF[0] = {};
    control.vuRegs.VF[0].F[3] = 1;
    std::memcpy(control.vuRegs.VF[30].F, now.data(), 16);
    std::memcpy(control.vuRegs.VF[29].F, next.data(), 16);
    std::memcpy(control.vuRegs.VF[31].F, carrier.data(), 16);
    const auto inputs = std::array<Math::Vector, 3>{now, next, carrier};
    MathLibraryJointLerpOracle::Invoke(control);
    const auto result = Math::Lerp(now, next, carrier[3]);
    const bool same = std::memcmp(result.data(), control.vuRegs.VF[30 - 1].F, 16) == 0;
    Check(same, "source inline scale blend differs from actual NEW macro arithmetic");
    Check(std::fegetround() == mode && result[3] == 1, "blend keeps caller rounding and publishes w one");
    const auto after = std::array<Math::Vector, 3>{now, next, carrier};
    Check(std::memcmp(inputs.data(), after.data(), sizeof(inputs)) == 0 &&
        std::memcmp(control.vuRegs.VF[30].F, now.data(), 16) == 0 &&
        std::memcmp(control.vuRegs.VF[31].F, carrier.data(), 16) == 0,
        "raw blend inputs remain unchanged");
    if (!same) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t a, b;
            std::memcpy(&a, &result[lane], 4); std::memcpy(&b, &control.vuRegs.VF[29].I[lane], 4);
            if (a != b) std::fprintf(stderr, "trial=%u lane=%u selected=%08x control=%08x\n", trial, lane, a, b);
        }
    }
    ++cases;
    return same;
}
}

int main() {
    const int previous = std::fegetround();
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "set joint blend rounding");
        uint32_t seed = 0x4c455250;
        for (unsigned trial = 0; trial < 8000; ++trial)
            if (!Blend(trial, seed, mode)) return 1;
    }
    std::fesetround(previous);
    std::printf("MathLibraryJointLerpOracle checks=%u failures=%u cases=%llu source=%s\n",
        checks, failures, static_cast<unsigned long long>(cases), MathLibraryJointLerpOracle::SourceDigest());
    return failures ? 1 : 0;
}
