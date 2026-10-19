#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "math_library_joint_image.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
using Math = title::JointMath;
unsigned checks = 0, failures = 0;
uint64_t original_calls = 0, selected_calls = 0, curved = 0, linear = 0;
void Check(bool good, const char* message) {
    ++checks;
    if (!good && failures++ < 16) std::fprintf(stderr, "%s\n", message);
}
uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
float Float(uint32_t bits) {
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}
uint32_t Bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, 4);
    return result;
}
void Input(Math::Vector& result, uint32_t& seed, bool raw) {
    double square = 0;
    for (auto& lane : result) {
        const auto bits = Next(seed);
        lane = raw ? Float(bits) : static_cast<float>(static_cast<int32_t>(bits)) / 2147483648.0f;
        square += static_cast<double>(lane) * lane;
    }
    if (!raw && square > 0) {
        const auto inverse = 1.0 / std::sqrt(square);
        for (auto& lane : result) lane = static_cast<float>(lane * inverse);
    }
}
void Pins() {
    const Math::Vector identity{0, 0, 0, 1};
    for (float share : {-2.0f, 0.0f, 0.5f, 1.0f, 3.0f}) {
        const auto result = Math::Interpolate(identity, identity, share);
        Check(!result.curved && result.rotation == identity,
            "identical unit keys remain identical for source unclamped shares");
    }
    const auto opposite = Math::Interpolate(identity, {0, 0, 0, -1}, 0.5f);
    Check(!opposite.curved && opposite.rotation == identity, "antipodal keys choose the shorter arc");
    const auto unnormalized = Math::Interpolate({0, 0, 0, 2}, {0, 0, 0, 3}, 2);
    Check(!unnormalized.curved && unnormalized.rotation[3] == 4,
        "source linear branch extrapolates without normalizing its result");
}
void Sequence(int mode) {
    VuUnit original(0);
    original.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    Math::Vector pose{Float(0xbcd903ab), Float(0xbac794af), Float(0x3c151e68), Float(0x3f7fe636)};
    const auto authored = pose;
    for (unsigned frame = 0; frame < 128; ++frame) {
        const Math::Vector target = frame < 32 || frame >= 96 ? authored : Math::Vector{0, 0.6f, 0, 0.8f};
        const float share = frame % 9 == 0 ? -0.25f : frame % 11 == 0 ? 1.25f : 0.125f;
        const Math::Vector shares{share, share, share, 1};
        std::memcpy(original.regs().VF[31].F, pose.data(), 16);
        std::memcpy(original.regs().VF[1].F, target.data(), 16);
        std::memcpy(original.regs().VF[29].F, shares.data(), 16);
        VuInterpreterTier control;
        ++original_calls;
        control.Run(original, 0x1c0);
        const auto before = original_calls;
        ++selected_calls;
        const auto selected = Math::Interpolate(pose, target, share);
        Check(std::memcmp(original.regs().VF[1].F, selected.rotation.data(), 16) == 0,
            "OLD R65/R66 feedback inputs and warm original state retain exact source results");
        Check(before == original_calls && std::fegetround() == mode,
            "history-free selected interpolation owns no original execution or rounding state");
        pose = selected.rotation;
    }
}
bool Compare(unsigned trial, int mode, uint32_t& seed) {
    Math::Vector from, to;
    Input(from, seed, trial % 5 == 0);
    Input(to, seed, trial % 5 == 0);
    float share = std::array<float, 10>{0, 1, 0.5f, -1, 2, 0.1f, 0.999f, 0.25f, -0.25f, 4}[trial % 10];
    if (trial % 11 == 0) share = Float(Next(seed));
    if (trial % 13 == 0) to = from;
    if (trial % 13 == 1) for (size_t lane = 0; lane < to.size(); ++lane) to[lane] = -from[lane];
    if (trial % 17 == 0) {
        from = {0, 0, 0, 1};
        to = {0, 0, 0, Float(0x3f7fbe77u + (trial / 17 % 3) - 1)};
    }
    if (trial % 37 == 0) {
        // R65/R66's two identical authored keys. The later R67 fix belongs to
        // signed track decoding; preserve this actual interpolation input too.
        from = {Float(0xbcd903ab), Float(0xbac794af), Float(0x3c151e68), Float(0x3f7fe636)};
        to = from;
    }
    VuUnit original(0);
    original.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    for (auto& row : original.regs().VF) for (auto& word : row.I) word = Next(seed);
    original.regs().Q = Next(seed);
    std::memcpy(original.regs().VF[31].F, from.data(), 16);
    std::memcpy(original.regs().VF[1].F, to.data(), 16);
    const Math::Vector shares{share, share, share, 1};
    std::memcpy(original.regs().VF[29].F, shares.data(), 16);
    VuInterpreterTier control;
    ++original_calls;  // Immediately at actual NEW operation execution.
    control.Run(original, 0x1c0);
    Check(original.regs().TPC == 0x548, "standard interpolation reaches its reviewed return");
    const auto before = original_calls;
    ++selected_calls;
    const auto selected = Math::Interpolate(from, to, share);
    if (selected.curved) ++curved; else ++linear;
    const auto& expected = original.regs().VF[1];
    const auto& weights = original.regs().VF[11];
    const bool same = std::memcmp(expected.F, selected.rotation.data(), 16) == 0 &&
        Bits(selected.to_weight) == weights.I[0] && Bits(selected.from_weight) == weights.I[2];
    Check(same, "source joint interpolation or weights differ from actual NEW");
    Check(before == original_calls, "selected interpolation executes no original model");
    Check(std::fegetround() == mode, "joint operation restores caller rounding");
    if (!same && failures < 16) {
        std::fprintf(stderr, "trial=%u mode=%d curved=%d share=%08x weights actual=%08x/%08x control=%08x/%08x\n",
            trial, mode, selected.curved, Bits(share), Bits(selected.to_weight), Bits(selected.from_weight),
            weights.I[0], weights.I[2]);
        for (size_t lane = 0; lane < 4; ++lane)
            std::fprintf(stderr, "lane=%zu from=%08x to=%08x actual=%08x control=%08x\n",
                lane, Bits(from[lane]), Bits(to[lane]), Bits(selected.rotation[lane]), expected.I[lane]);
    }
    return same;
}
}

int main() {
    const int previous = std::fegetround();
    Pins();
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "set joint test rounding");
        Sequence(mode);
        uint32_t seed = 0x4a4f494e;
        for (unsigned trial = 0; trial < 4000; ++trial)
            if (!Compare(trial, mode, seed)) return 1;
    }
    std::fesetround(previous);
    Check(curved > 0 && linear > 0, "both source interpolation branches receive positive work");
    std::printf("MathLibraryJointOracle checks=%u failures=%u original=%llu selected=%llu curved=%llu linear=%llu\n",
        checks, failures, static_cast<unsigned long long>(original_calls),
        static_cast<unsigned long long>(selected_calls), static_cast<unsigned long long>(curved),
        static_cast<unsigned long long>(linear));
    return failures ? 1 : 0;
}
