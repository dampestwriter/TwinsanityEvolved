#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_trig_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle_artifacts.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

#include "engine/vu/vu0/lib_vu0_sin_cos.h"
#include "engine/vu/vu_chop_round_guard.h"
#include "games/slus_20909_crash_twinsanity/hle/math/trig_math.h"

uint8_t* MathLibraryTrigOracle::Memory::Translate(uint32_t address, size_t length) {
    address = (address & 0x70000000u) == 0x70000000u
        ? 4096 + (address & 0x3fffu) : address & 0x1fffffffu;
    if (address > bytes.size() || length > bytes.size() - address) std::abort();
    return bytes.data() + address;
}

uint32_t MathLibraryTrigOracle::Memory::Read32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, Translate(address, 4), 4);
    return value;
}

void MathLibraryTrigOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, 4), &value, 4);
}

namespace {
uint64_t model_calls = 0;
unsigned checks = 0;
unsigned failures = 0;
uint64_t selected_entries = 0, control_bodies = 0;
using Oracle = MathLibraryTrigOracle;
using Math = title::TrigMath;

const auto& Program() {
    // This exact immutable fixture is both consumed by NEW and exported.
    // Bytes come only from NEW's public signature, never a captured image.
    static const auto code = [] {
        std::array<uint8_t, LibVu0SinCos::kEntryPc + sizeof(LibVu0SinCos::kFingerprint)> bytes{};
        std::memcpy(bytes.data() + LibVu0SinCos::kEntryPc, LibVu0SinCos::kFingerprint,
            sizeof(LibVu0SinCos::kFingerprint));
        return bytes;
    }();
    return code;
}

void Check(bool pass, const char* message) {
    ++checks;
    if (pass) return;
    ++failures;
    std::fprintf(stderr, "Trig pin FAIL: %s\n", message);
}

uint32_t Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

void ValuePins() {
    constexpr int modes[] = {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD};
    for (const int mode : modes) {
        Check(std::fesetround(mode) == 0, "set rounding");
        auto values = Math::EvaluateSinCos(0, -0.0f);
        Check(Bits(values[0]) == 0 && values[1] == 1 && Bits(values[2]) == 0x80000000u &&
            values[3] == 1, "signed zeros");
        values = Math::EvaluateSinCos(Float(0x0000807f), Float(0x8000807f));
        Check(Bits(values[0]) == 0 && Bits(values[2]) == 0x80000000u, "denormal inputs retain sign");
        const auto saturated = Math::EvaluateSinCos(Float(0x7f7fffff), Float(0xff7fffff));
        const auto exceptional = Math::EvaluateSinCos(Float(0x7fc12345), Float(0xff800000));
        Check(std::memcmp(saturated.data(), exceptional.data(), 16) == 0, "nonfinite saturation");
        Check(std::isfinite(exceptional[0]) && std::isfinite(exceptional[3]), "large argument stays finite");
        // R67's negative angle/wrap class: -32768 remains a signed half turn,
        // not a masked positive input or an added full revolution.
        Check(Math::AngleRadians(-32768) < -3 && Math::AngleRadians(-1) < 0 &&
            Math::AngleRadians(32768) > 3, "signed angle boundaries");
        for (auto axis : {Math::Axis::Pitch, Math::Axis::Yaw, Math::Axis::Roll}) {
            const auto zero = Math::EvaluateRotation(0, axis);
            Check(zero == Math::Vector{0, 0, 0, 1}, "zero rotation identity");
            const auto quarter = Math::EvaluateRotation(16384, axis);
            const auto half = Math::EvaluateRotation(-32768, axis);
            const size_t component = static_cast<size_t>(axis);
            Check(std::fabs(quarter[component] - 0.70710678f) < 0.000001f &&
                std::fabs(quarter[3] - 0.70710678f) < 0.000001f, "quarter turn quaternion");
            Check(std::fabs(half[component] + 1) < 0.000001f &&
                std::fabs(half[3]) < 0.000001f, "negative half turn quaternion");
            Check(Math::EvaluateAxisMatrix(0, axis) == title::MathLibrary::EvaluateIdentity(),
                "zero matrix identity");
            Oracle::Memory identity_memory;
            CpuContext identity_context{};
            identity_context.vuRegs.VF[0].w = 1;
            identity_context.cpuRegs.GPR.r[4].UL[0] = 0x200;
            identity_context.cpuRegs.GPR.r[5].UL[0] = 0x100;
            identity_context.cpuRegs.GPR.r[29].UL[0] = 0x780;
            Oracle::Invoke(Oracle::Entries()[9 + component].address, identity_context, identity_memory);
            const auto identity = Math::EvaluateAxisMatrix(0, axis);
            Check(std::memcmp(identity.data(), identity_memory.Translate(0x200, 64), 64) == 0,
                "identity value retains source clear-row signed zeros in each rounding mode");
            const auto matrix = Math::EvaluateAxisMatrix(16384, axis);
            const auto negative = Math::EvaluateAxisMatrix(-16384, axis);
            const size_t first = axis == Math::Axis::Pitch ? 1 : 0;
            const size_t second = axis == Math::Axis::Roll ? 1 : 2;
            const float sign = axis == Math::Axis::Yaw ? -1.0f : 1.0f;
            Check(std::fabs(matrix[first][second] - sign) < 0.000001f &&
                std::fabs(matrix[second][first] + sign) < 0.000001f, "matrix quarter-turn orientation");
            Check(std::fabs(negative[first][second] + sign) < 0.000001f &&
                std::fabs(negative[second][first] - sign) < 0.000001f, "matrix signed negative turn");
            Check(std::fabs(matrix[first][first]) < 0.000001f &&
                std::fabs(matrix[second][second]) < 0.000001f, "matrix rotating diagonal");
            Check(matrix[component][component] == 1 && matrix[3] == Math::Vector{0, 0, 0, 1},
                "matrix fixed axis and translation row");
        }
        for (auto axes : {Math::AxisPair::XY, Math::AxisPair::XZ, Math::AxisPair::YZ}) {
            const auto zero = Math::EvaluateRotationPair(0, 0, axes);
            Check(zero == Math::Vector{0, 0, 0, 1}, "paired zero identity");
            const auto first = axes == Math::AxisPair::YZ ? Math::Axis::Yaw : Math::Axis::Pitch;
            const auto second = axes == Math::AxisPair::XY ? Math::Axis::Yaw : Math::Axis::Roll;
            Check(Math::EvaluateRotationPair(-32768, 0, axes) == Math::EvaluateRotation(-32768, first),
                "paired first-only fast path");
            Check(Math::EvaluateRotationPair(0, -32768, axes) == Math::EvaluateRotation(-32768, second),
                "paired second-only fast path");
            const auto both = Math::EvaluateRotationPair(16384, 16384, axes);
            for (size_t lane = 0; lane < 4; ++lane) {
                const bool negative = (axes == Math::AxisPair::XY && lane == 2) ||
                    (axes == Math::AxisPair::YZ && lane == 0);
                Check(std::fabs(both[lane] - (negative ? -0.5f : 0.5f)) < 0.000001f,
                    "paired quarter-turn composition and cross sign");
            }
        }
        Check(Math::EvaluateRotationXYZ(0, -32768, 16384) ==
            Math::EvaluateRotationPair(-32768, 16384, Math::AxisPair::YZ), "XYZ zero-x closure");
        Check(Math::EvaluateRotationXYZ(-32768, 0, 16384) ==
            Math::EvaluateRotationPair(-32768, 16384, Math::AxisPair::XZ), "XYZ zero-y closure");
        Check(Math::EvaluateRotationXYZ(-32768, 16384, 0) ==
            Math::EvaluateRotationPair(-32768, 16384, Math::AxisPair::XY), "XYZ zero-z closure");
        const auto xyz = Math::EvaluateRotationXYZ(16384, 16384, 16384);
        Check(std::fabs(xyz[0]) < 0.000001f && std::fabs(xyz[2]) < 0.000001f &&
            std::fabs(xyz[1] - 0.70710678f) < 0.000001f &&
            std::fabs(xyz[3] - 0.70710678f) < 0.000001f, "XYZ three-axis orientation");
        uint32_t seed = 0x58595a;
        for (unsigned trial = 0; trial < 512; ++trial) {
            Oracle::Memory memory;
            CpuContext context{};
            context.cpuRegs.GPR.r[4].UL[0] = 0x200;
            context.cpuRegs.GPR.r[29].UL[0] = 0x780;
            std::array<int32_t, 3> angles;
            for (unsigned axis = 0; axis < 3; ++axis) {
                angles[axis] = trial & (1u << axis) ? static_cast<int32_t>(Next(seed)) : 0;
                context.cpuRegs.GPR.r[5 + axis].UL[0] = 0x100 + 4 * axis;
                memory.Write32(0x100 + 4 * axis, static_cast<uint32_t>(angles[axis]));
            }
            const auto value = Math::EvaluateRotationXYZ(angles[0], angles[1], angles[2]);
            Oracle::Invoke(Math::kRotationXYZAddress, context, memory);
            Check(std::memcmp(value.data(), memory.Translate(0x200, 16), 16) == 0,
                "XYZ source value operation agrees with NEW full-entry result");
        }
        Check(std::fegetround() == mode, "operation restores caller rounding");
    }
}

void TangentPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "tangent set mode");
        unsigned nearest_differs_from_chop = 0;
        for (int32_t angle = -256; angle < 256; ++angle) {
            Oracle::Memory memory;
            CpuContext expected{};
            expected.cpuRegs.GPR.r[4].UL[0] = 0x100;
            expected.cpuRegs.GPR.r[29].UL[0] = 0x710;
            memory.Write32(0x100, static_cast<uint32_t>(angle * 71));
            const float value = Math::EvaluateTangent(angle * 71);
            Oracle::Invoke(Math::kTangentAddress, expected, memory);
            Check(Bits(value) == expected.fpuRegs.fpr[0].UL, "tangent value API matches actual NEW return");
            const auto values = Math::EvaluateSinCos(Math::AngleRadians(angle * 71), 0);
            (void)std::fesetround(FE_TOWARDZERO);
            volatile float chopped = values[0] / values[1];
            (void)std::fesetround(mode);
            if (Bits(chopped) != Bits(value)) ++nearest_differs_from_chop;
        }
        Check(nearest_differs_from_chop > 0, "scalar tangent division keeps OLD nearest correction");
        // Unaligned scalar scratch reads expose a zero cosine and a signed
        // nonzero sine. This pins the source finite sentinel branch directly,
        // without assuming libm returns exact zero at a nominal quarter turn.
        for (const bool negative : {false, true}) {
            Oracle::Memory memory, expected_memory;
            CpuContext actual{}, expected;
            actual.cpuRegs.GPR.r[4].UL[0] = 0x100;
            actual.cpuRegs.GPR.r[29].UL[0] = 0x714;
            actual.fpuRegs.fprc[31] = 0xdeadbeef;
            memory.Write32(0x700, negative ? 0x40490fdb : 0);
            expected = actual;
            expected_memory = memory;
            Math math(memory);
            math.TanOfAngle(actual);
            Oracle::Invoke(Math::kTangentAddress, expected, expected_memory);
            Check(actual.fpuRegs.fpr[0].UL == (negative ? 0xf149f2cau : 0x7149f2cau),
                "tangent source +/-1e30 sentinel");
            Check(std::memcmp(&actual, &expected, sizeof(actual)) == 0 &&
                memory.bytes == expected_memory.bytes, "tangent sentinel context flags and scratch");
        }
        Check(std::fegetround() == mode, "tangent restores caller rounding");
    }
}

void MatrixPairPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "paired matrix set mode");
        uint32_t seed = 0x4d415450;
        for (const auto axes : {Math::AxisPair::XY, Math::AxisPair::XZ, Math::AxisPair::YZ}) {
            const auto first_axis = axes == Math::AxisPair::YZ ? Math::Axis::Yaw : Math::Axis::Pitch;
            const auto second_axis = axes == Math::AxisPair::XY ? Math::Axis::Yaw : Math::Axis::Roll;
            Check(Math::EvaluateMatrixPair(-32768, 0, axes) == Math::EvaluateAxisMatrix(-32768, first_axis),
                "paired matrix first-only source path");
            Check(Math::EvaluateMatrixPair(0, -32768, axes) == Math::EvaluateAxisMatrix(-32768, second_axis),
                "paired matrix second-only source path");
            Check(Math::EvaluateMatrixPair(0, 0, axes) == title::MathLibrary::EvaluateIdentity(),
                "paired matrix identity source path");
            bool reported = false;
            for (unsigned trial = 0; trial < 512; ++trial) {
                Oracle::Memory memory;
                CpuContext expected{};
                // A matrix value operation uses canonical identity constants.
                // The full adapter oracle separately covers arbitrary raw VF0.
                expected.vuRegs.VF[0].w = 1;
                expected.cpuRegs.GPR.r[4].UL[0] = 0x200;
                expected.cpuRegs.GPR.r[5].UL[0] = 0x100;
                expected.cpuRegs.GPR.r[6].UL[0] = 0x104;
                expected.cpuRegs.GPR.r[29].UL[0] = 0x780;
                const int32_t first = trial & 1 ? static_cast<int32_t>(Next(seed)) : 0;
                const int32_t second = trial & 2 ? static_cast<int32_t>(Next(seed)) : 0;
                memory.Write32(0x100, static_cast<uint32_t>(first));
                memory.Write32(0x104, static_cast<uint32_t>(second));
                const auto value = Math::EvaluateMatrixPair(first, second, axes);
                Oracle::Invoke(Oracle::Entries()[14 + static_cast<size_t>(axes)].address, expected, memory);
                const bool same = std::memcmp(value.data(), memory.Translate(0x200, 64), 64) == 0;
                Check(same, "paired matrix source value agrees with NEW output");
                if (!same && !reported) {
                    reported = true;
                    std::fprintf(stderr, "Matrix value mismatch mode=%d axes=%u first=%d second=%d\n",
                        mode, static_cast<unsigned>(axes), first, second);
                    for (unsigned i = 0; i < 16; ++i) {
                        const auto actual = Bits(value[i / 4][i % 4]), model = memory.Read32(0x200 + i * 4);
                        if (actual != model) std::fprintf(stderr, " matrix[%u] actual=%08x model=%08x\n",
                            i, actual, model);
                    }
                }
            }
        }
        Check(std::fegetround() == mode, "paired matrix restores caller rounding");
    }
}

void MatrixXYZPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "XYZ matrix set mode");
        uint32_t seed = 0x58594d;
        for (unsigned trial = 0; trial < 512; ++trial) {
            Oracle::Memory memory;
            CpuContext expected{};
            expected.vuRegs.VF[0].w = 1;
            expected.cpuRegs.GPR.r[4].UL[0] = 0x200;
            expected.cpuRegs.GPR.r[29].UL[0] = 0x780;
            std::array<int32_t, 3> angles;
            for (unsigned axis = 0; axis < 3; ++axis) {
                angles[axis] = trial & (1u << axis) ? static_cast<int32_t>(Next(seed)) : 0;
                expected.cpuRegs.GPR.r[5 + axis].UL[0] = 0x100 + 4 * axis;
                memory.Write32(0x100 + 4 * axis, static_cast<uint32_t>(angles[axis]));
            }
            const auto value = Math::EvaluateMatrixXYZ(angles[0], angles[1], angles[2]);
            Oracle::Invoke(Math::kMatrixXYZAddress, expected, memory);
            Check(std::memcmp(value.data(), memory.Translate(0x200, 64), 64) == 0,
                "XYZ matrix value matches actual NEW output including zero-axis closures");
        }
        Check(std::fegetround() == mode, "XYZ matrix restores caller rounding");
    }
}

void TurnTowardPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "direction turn set mode");
        const Math::Vector from{1, 0, 0, Float(0x0000807f)}, across{0, 1, 0, Float(0x8000807f)};
        const auto half = Math::EvaluateTurnToward(0.5f, from, across);
        Check(std::fabs(half[0] - 0.70710678f) < 0.0001f &&
            std::fabs(half[1] - 0.70710678f) < 0.0001f && half[2] == 0,
            "source approximate quarter-angle interpolation");
        Check(Bits(half[3]) == 0x0000807f, "nonparallel turn preserves raw from.w payload");
        const Math::Vector parallel{1, 0, 0, Float(0x8000807f)};
        const auto unchanged = Math::EvaluateTurnToward(0.25f, from, parallel);
        Check(std::memcmp(unchanged.data(), parallel.data(), 16) == 0,
            "zero-sine branch copies all of to including raw w");
        uint32_t seed = 0x5455524e;
        for (unsigned trial = 0; trial < 512; ++trial) {
            Oracle::Memory memory;
            CpuContext expected{};
            expected.vuRegs.VF[0].w = 1;
            expected.cpuRegs.GPR.r[4].UL[0] = 0x300;
            expected.cpuRegs.GPR.r[5].UL[0] = 0x100;
            expected.cpuRegs.GPR.r[6].UL[0] = 0x200;
            expected.cpuRegs.GPR.r[29].UL[0] = 0x780;
            Math::Vector first{}, second{};
            for (unsigned lane = 0; lane < 3; ++lane) {
                first[lane] = static_cast<float>(static_cast<int32_t>(Next(seed) % 2001) - 1000) / 1024.0f;
                second[lane] = static_cast<float>(static_cast<int32_t>(Next(seed) % 2001) - 1000) / 1024.0f;
            }
            first[3] = Float(Next(seed));
            second[3] = Float(Next(seed));
            const float share = static_cast<float>(static_cast<int32_t>(trial % 9) - 2) / 4.0f;
            expected.fpuRegs.fpr[12].f = share;
            std::memcpy(memory.Translate(0x100, 16), first.data(), 16);
            std::memcpy(memory.Translate(0x200, 16), second.data(), 16);
            const auto value = Math::EvaluateTurnToward(share, first, second);
            Oracle::Invoke(Math::kTurnTowardAddress, expected, memory);
            Check(std::memcmp(value.data(), memory.Translate(0x300, 16), 16) == 0,
                "direction turn value matches NEW cubic, nearest root/division and separate products");
        }
        Check(std::fegetround() == mode, "direction turn restores caller rounding");
    }
}

void AxisTurnPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "axis turn set mode");
        const Math::Vector vector{1, 0, 0, Float(0x0000807f)}, axis{0, 0, 2, Float(0x8000807f)};
        const auto quarter = Math::EvaluateTurnBySineCosine(1, 0, vector, axis, false);
        Check(quarter[0] == 0 && quarter[1] == 1 && quarter[2] == 0 &&
            Bits(quarter[3]) == 0x0000807f, "Rodrigues normalizes axis and preserves raw vector w");
        const Math::Vector tiny{0, 0, 1e-7f, 0}, zero{};
        for (const auto& small : {tiny, zero}) {
            const auto result = Math::EvaluateTurnBySineCosine(1, 0, vector, small, false);
            Check(std::memcmp(result.data(), vector.data(), 16) == 0,
                "source degenerate-axis threshold leaves the entire vector unchanged");
        }
        const auto trusted_zero = Math::EvaluateTurnBySineCosine(1, 0, vector, zero, true);
        Check(trusted_zero[0] == 0 && trusted_zero[1] == 0 && trusted_zero[2] == 0 &&
            Bits(trusted_zero[3]) == 0x0000807f, "unit-axis flag bypasses normalization and degeneracy test");
        const auto negative = Math::EvaluateTurnAboutAxis(-16384, vector, axis, false);
        Check(std::fabs(negative[0]) < 0.0001f && negative[1] < -0.9999f,
            "negative quarter turn keeps signed direction");
        for (unsigned operation = 19; operation < 21; ++operation) {
            uint32_t seed = 0x41584953;
            for (unsigned trial = 0; trial < 512; ++trial) {
                Oracle::Memory memory;
                CpuContext expected{};
                expected.cpuRegs.GPR.r[4].UL[0] = 0x100;
                expected.cpuRegs.GPR.r[5].UL[0] = 0x200;
                expected.cpuRegs.GPR.r[29].UL[0] = 0x780;
                Math::Vector value{}, direction{};
                for (unsigned lane = 0; lane < 3; ++lane) {
                    value[lane] = static_cast<float>(static_cast<int32_t>(Next(seed) % 2001) - 1000) / 128.0f;
                    direction[lane] = static_cast<float>(static_cast<int32_t>(Next(seed) % 2001) - 1000) / 1024.0f;
                }
                if (trial % 7 == 0) direction = trial % 2 ? tiny : zero;
                value[3] = Float(Next(seed));
                direction[3] = Float(Next(seed));
                const bool unit = trial % 3 == 0;
                const int32_t angle = trial % 4 ? static_cast<int32_t>(Next(seed)) : 0;
                const auto pair = Math::EvaluateSinCos(Math::AngleRadians(angle), 0);
                std::memcpy(memory.Translate(0x100, 16), value.data(), 16);
                std::memcpy(memory.Translate(0x200, 16), direction.data(), 16);
                Math::Vector result;
                if (operation == 19) {
                    expected.cpuRegs.GPR.r[6].UD[0] = unit;
                    expected.fpuRegs.fpr[12].f = pair[0];
                    expected.fpuRegs.fpr[13].f = pair[1];
                    result = Math::EvaluateTurnBySineCosine(pair[0], pair[1], value, direction, unit);
                } else {
                    expected.cpuRegs.GPR.r[6].UL[0] = 0x300;
                    expected.cpuRegs.GPR.r[7].UD[0] = unit;
                    memory.Write32(0x300, static_cast<uint32_t>(angle));
                    result = Math::EvaluateTurnAboutAxis(angle, value, direction, unit);
                }
                Oracle::Invoke(Oracle::Entries()[operation].address, expected, memory);
                Check(std::memcmp(result.data(), memory.Translate(0x100, 16), 16) == 0,
                    "axis-turn value agrees with actual NEW closure under every caller mode");
            }
        }
        Check(std::fegetround() == mode, "axis turn restores caller rounding");
    }
}

void AxisRotationPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "axis quaternion set mode");
        const Math::Vector axis{0, 0, 2, Float(0x807f)};
        for (const int32_t odd : {-1, 1}) {
            const auto value = Math::EvaluateRotationAboutAxis(odd, axis, false);
            const Math::Vector identity{0, 0, 0, 1};
            Check(std::memcmp(value.data(), identity.data(), 16) == 0,
                "source odd half-angle truncates to integer zero before radians");
        }
        const auto negative = Math::EvaluateRotationAboutAxis(-16384, axis, false);
        Check(negative[2] < -0.707f && negative[3] > 0.707f,
            "signed negative half angle keeps quaternion direction");
        for (const uint32_t payload : {0x0000807fu, 0x8000807fu, 0x7fc12345u}) {
            const auto value = Math::EvaluateRotationFromAxisSine(0.5f, Float(payload), axis, false);
            Check(Bits(value[3]) == payload, "quaternion scalar is the raw cosine payload");
        }
        Oracle::Memory alias_memory;
        CpuContext alias{};
        alias.cpuRegs.GPR.r[4].UL[0] = alias.cpuRegs.GPR.r[5].UL[0] = 0x100;
        alias.cpuRegs.GPR.r[6].UD[0] = 1;
        alias.fpuRegs.fpr[12].f = 0.5f;
        alias.fpuRegs.fpr[13].f = -0.25f;
        const Math::Vector original_axis{3, 4, 5, 6}, alias_result{0.25f, 0.25f, 0.25f, -0.25f};
        std::memcpy(alias_memory.Translate(0x100, 16), original_axis.data(), 16);
        Math alias_math(alias_memory);
        alias_math.RotationFromAxisSine(alias);
        Check(std::memcmp(alias_memory.Translate(0x100, 16), alias_result.data(), 16) == 0,
            "complete ABI publishes scale before reading an aliased axis");
        for (unsigned operation = 21; operation < 23; ++operation) {
            uint32_t seed = 0x51554154;
            for (unsigned trial = 0; trial < 512; ++trial) {
                Oracle::Memory memory;
                CpuContext expected{};
                expected.cpuRegs.GPR.r[4].UL[0] = 0x100;
                expected.cpuRegs.GPR.r[5].UL[0] = 0x200;
                expected.cpuRegs.GPR.r[29].UL[0] = 0x780;
                Math::Vector direction{};
                for (unsigned lane = 0; lane < 3; ++lane)
                    direction[lane] = static_cast<float>(static_cast<int32_t>(Next(seed) % 2001) - 1000) / 1024.0f;
                if (trial % 7 == 0) direction = {};
                // Sweep either side of the source normalization threshold.
                if (trial % 11 == 0) direction = {0, 0, Float(0x3851b715u + trial % 5), 0};
                direction[3] = Float(Next(seed));
                std::memcpy(memory.Translate(0x200, 16), direction.data(), 16);
                const bool unit = trial % 3 == 0;
                const int32_t angle = trial % 5 ? static_cast<int32_t>(Next(seed)) :
                    static_cast<int32_t>(trial % 7) - 3;
                const auto pair = Math::EvaluateSinCos(Math::AngleRadians(angle), 0);
                Math::Vector result;
                if (operation == 21) {
                    expected.cpuRegs.GPR.r[6].UD[0] = unit;
                    expected.fpuRegs.fpr[12].f = pair[0];
                    expected.fpuRegs.fpr[13].f = pair[1];
                    result = Math::EvaluateRotationFromAxisSine(pair[0], pair[1], direction, unit);
                } else {
                    expected.cpuRegs.GPR.r[6].UL[0] = 0x300;
                    expected.cpuRegs.GPR.r[7].UD[0] = unit;
                    memory.Write32(0x300, static_cast<uint32_t>(angle));
                    result = Math::EvaluateRotationAboutAxis(angle, direction, unit);
                }
                Oracle::Invoke(Oracle::Entries()[operation].address, expected, memory);
                Check(std::memcmp(result.data(), memory.Translate(0x100, 16), 16) == 0,
                    "axis-quaternion value agrees with actual NEW complete closure");
            }
        }
        Check(std::fegetround() == mode, "axis quaternion restores caller rounding");
    }
}

void AxisMatrixPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "arbitrary-axis matrix set mode");
        const Math::Vector axis{0, 0, 2, Float(0x7fc12345)};
        const auto quarter = Math::EvaluateMatrixAboutAxis(1, 0, axis, false);
        Check(quarter[0][1] == 1 && quarter[1][0] == -1 && quarter[2][2] == 1 &&
            quarter[3][3] == 1, "axis matrix normalizes and uses source row orientation");
        const auto degenerate = Math::EvaluateMatrixAboutAxis(1, 0.5f, {}, false);
        Check(degenerate[0][0] == 0.5f && degenerate[1][1] == 0.5f && degenerate[2][2] == 0.5f &&
            degenerate[3][3] == 1, "degenerate source matrix retains cosine diagonal");
        Oracle::Memory zero_memory;
        CpuContext zero_context{};
        zero_context.vuRegs.VF[0].w = 1;
        zero_context.cpuRegs.GPR.r[4].UL[0] = 0x100;
        zero_context.cpuRegs.GPR.r[5].UL[0] = 0xdeadbeefu;
        zero_context.cpuRegs.GPR.r[6].UL[0] = 0x300;
        zero_context.cpuRegs.GPR.r[29].UL[0] = 0x780;
        auto expected_zero = zero_context;
        auto expected_memory = zero_memory;
        Math zero_math(zero_memory);
        zero_math.AxisAngleMatrix(zero_context);
        Oracle::Invoke(Math::kMatrixAxisAddress, expected_zero, expected_memory);
        Check(zero_memory.bytes == expected_memory.bytes &&
            std::memcmp(&zero_context, &expected_zero, sizeof(zero_context)) == 0,
            "zero-angle identity does not touch an unbacked axis pointer");
        Check(zero_math.Counts().entries[24] == 1 && zero_math.Counts().entries[23] == 0,
            "zero-angle identity bypasses the source axis-matrix leaf");
        for (unsigned operation = 23; operation < 25; ++operation) {
            uint32_t seed = 0x4d415849;
            for (unsigned trial = 0; trial < 512; ++trial) {
                Oracle::Memory memory;
                CpuContext expected{};
                expected.vuRegs.VF[0].w = 1;
                expected.cpuRegs.GPR.r[4].UL[0] = 0x100;
                expected.cpuRegs.GPR.r[5].UL[0] = 0x200;
                expected.cpuRegs.GPR.r[29].UL[0] = 0x780;
                Math::Vector direction{};
                for (unsigned lane = 0; lane < 3; ++lane)
                    direction[lane] = static_cast<float>(static_cast<int32_t>(Next(seed) % 2001) - 1000) / 1024.0f;
                if (trial % 7 == 0) direction = {};
                if (trial % 11 == 0) direction = {0, 0, Float(0x3851b715u + trial % 5), 0};
                direction[3] = Float(Next(seed));
                std::memcpy(memory.Translate(0x200, 16), direction.data(), 16);
                const bool unit = trial % 3 == 0;
                const int32_t angle = trial % 4 ? static_cast<int32_t>(Next(seed)) : 0;
                const auto pair = Math::EvaluateSinCos(Math::AngleRadians(angle), 0);
                Math::Matrix result;
                if (operation == 23) {
                    expected.cpuRegs.GPR.r[6].UD[0] = unit;
                    expected.fpuRegs.fpr[12].f = pair[0];
                    expected.fpuRegs.fpr[13].f = pair[1];
                    result = Math::EvaluateMatrixAboutAxis(pair[0], pair[1], direction, unit);
                } else {
                    expected.cpuRegs.GPR.r[6].UL[0] = 0x300;
                    expected.cpuRegs.GPR.r[7].UD[0] = unit;
                    memory.Write32(0x300, static_cast<uint32_t>(angle));
                    result = Math::EvaluateAxisAngleMatrix(angle, direction, unit);
                }
                Oracle::Invoke(Oracle::Entries()[operation].address, expected, memory);
                Check(std::memcmp(result.data(), memory.Translate(0x100, 64), 64) == 0,
                    "arbitrary-axis matrix value matches actual NEW identity or scalar closure");
            }
        }
        Check(std::fegetround() == mode, "arbitrary-axis matrix restores caller rounding");
    }
}

void InverseLengthPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "inverse length set mode");
        const Math::Vector five{3, 0, 0, 4};
        Check(Bits(Math::EvaluateInverseLength4(0, 0, five)) == 0x3e4ccccdu,
            "single-precision nearest inverse length preserves one-fifth bit pin");
        Check(Bits(Math::EvaluateInverseLength4(Float(0x807f), 5, five)) == 0x807fu,
            "equal squared threshold returns the raw fallback");
        Check(Bits(Math::EvaluateInverseLength4(Float(0x7fc12345), 1, {})) == 0x7fc12345u,
            "zero length preserves nonfinite fallback bits");
        Check(Bits(Math::EvaluateInverseLength4(0, Float(0x2edbe6ff), {0.5f, 0, 0, 0})) == 0x40000000u,
            "OLD R63 half-length quaternion remains a valid normalization input");
        uint32_t seed = 0x494e5634;
        for (unsigned trial = 0; trial < 512; ++trial) {
            Math::Vector vector;
            for (auto& lane : vector) lane = Float(Next(seed));
            if (trial % 8 == 0) vector = {};
            if (trial % 8 == 1) vector = five;
            if (trial % 8 == 2) vector = {0, 0, 0, 5};
            if (trial % 8 == 3) vector = {Float(0x2edbe6feu + trial % 3), 0, 0, 0};
            const float epsilon = trial % 8 == 1 ? 5.0f :
                trial % 8 == 3 ? Float(0x2edbe6ff) : trial % 2 ? Float(Next(seed)) : 0.0f;
            const float fallback = Float(Next(seed));
            Oracle::Memory memory;
            CpuContext expected{};
            expected.cpuRegs.GPR.r[4].UL[0] = 0x100;
            expected.fpuRegs.fpr[12].f = fallback;
            expected.fpuRegs.fpr[13].f = epsilon;
            std::memcpy(memory.Translate(0x100, 16), vector.data(), 16);
            const float result = Math::EvaluateInverseLength4(fallback, epsilon, vector);
            Oracle::Invoke(Math::kInverseLength4Address, expected, memory);
            Check(Bits(result) == expected.fpuRegs.fpr[0].UL,
                "typed inverse length matches actual NEW reciprocal root and threshold");
        }
        Check(std::fegetround() == mode, "inverse length restores caller rounding");
    }
}

void SlerpPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "slerp set mode");
        const Math::Vector identity{0, 0, 0, 1}, quarter{0, 0.70710678f, 0, 0.70710678f};
        const auto halfway = Math::EvaluateSlerp(0.5f, identity, quarter);
        Check(std::fabs(halfway[1] - 0.38268343f) < 0.0003f &&
            std::fabs(halfway[3] - 0.92387953f) < 0.0003f, "source curved interpolation midpoint");
        const auto extrapolated = Math::EvaluateSlerp(2, identity, quarter);
        Check(extrapolated[1] > 0.999f && std::fabs(extrapolated[3]) < 0.001f,
            "curved source path extrapolates rather than clamping share");
        const Math::Vector from{1, 0, -0.0f, Float(0x807f)}, to{1, -0.0f, 0, Float(0x8000807f)};
        for (const float share : {-1.0f, 0.0f, Float(0x7fc12345)}) {
            const auto result = Math::EvaluateSlerp(share, from, to);
            Check(std::memcmp(result.data(), from.data(), 16) == 0,
                "straight source lower endpoint preserves raw from payload");
        }
        for (const float share : {1.0f, 2.0f}) {
            const auto result = Math::EvaluateSlerp(share, from, to);
            Check(std::memcmp(result.data(), to.data(), 16) == 0,
                "straight source upper endpoint preserves raw to payload");
        }
        const auto opposite = Math::EvaluateSlerp(0.5f, identity, {0, 0, 0, -1});
        Check(opposite[3] == -1, "shorter arc selects from's opposite quaternion");
        uint32_t seed = 0x534c4552;
        for (unsigned trial = 0; trial < 512; ++trial) {
            auto first = Math::EvaluateRotationXYZ(static_cast<int32_t>(Next(seed)),
                static_cast<int32_t>(Next(seed)), static_cast<int32_t>(Next(seed)));
            auto second = Math::EvaluateRotationXYZ(static_cast<int32_t>(Next(seed)),
                static_cast<int32_t>(Next(seed)), static_cast<int32_t>(Next(seed)));
            if (trial % 5 == 0) second = first;
            if (trial % 5 == 1) for (unsigned lane = 0; lane < 4; ++lane) second[lane] = -first[lane];
            if (trial % 5 == 2) {
                first = identity;
                const float cosine = Float(0x3f733332u + trial % 3);
                second = {0, std::sqrt(1 - cosine * cosine), 0, cosine};
            }
            constexpr uint32_t shares[] = {0xbf800000, 0, 0x3e800000, 0x3f000000, 0x3f7fffff,
                0x3f800000, 0x40000000, 0x7fc12345, 0x0000807f};
            const float share = Float(shares[trial % 9]);
            Oracle::Memory memory;
            CpuContext expected{};
            expected.cpuRegs.GPR.r[4].UL[0] = 0x300;
            expected.cpuRegs.GPR.r[5].UL[0] = 0x100;
            expected.cpuRegs.GPR.r[6].UL[0] = 0x200;
            expected.cpuRegs.GPR.r[29].UL[0] = 0x780;
            expected.fpuRegs.fpr[12].f = share;
            std::memcpy(memory.Translate(0x100, 16), first.data(), 16);
            std::memcpy(memory.Translate(0x200, 16), second.data(), 16);
            const auto result = Math::EvaluateSlerp(share, first, second);
            Oracle::Invoke(Math::kSlerpAddress, expected, memory);
            Check(std::memcmp(result.data(), memory.Translate(0x300, 16), 16) == 0,
                "typed quaternion interpolation matches actual NEW complete closure");
        }
        Check(std::fegetround() == mode, "slerp restores caller rounding");
    }
}

void AnglePins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "angle conversion set mode");
        Check(Math::EvaluateAngleFrom(0.5f, 2) == 32768 &&
            Math::EvaluateAngleFrom(-0.5f, 2) == -32768, "signed half-turn conversion");
        Check(Math::EvaluateAngleFrom(180, 1) == 32768 &&
            Math::EvaluateAngleFrom(-180, 1) == -32768, "degrees retain signed angle boundaries");
        Check(Math::EvaluateAngleFrom(-1, 99) == Math::EvaluateAngleFrom(-1, 0),
            "unknown angle unit uses radians");
        Check(Math::EvaluateAngleFrom(Float(0x7f800000), 2) == 2147483647 &&
            Math::EvaluateAngleFrom(Float(0xff800000), 2) == -2147483647 - 1,
            "source conversion saturates at signed integer limits");
        Check(Math::EvaluateAngleOfPoint(0, 0) == 32768 &&
            Math::EvaluateAngleOfPoint(0, -1) == 32768,
            "zero beyond and negative x retain source half-turn result");
        Check(Math::EvaluateAngleOfPoint(0, 1) == 0, "positive x is zero angle");
        const int32_t up = Math::EvaluateAngleOfPoint(1, 0);
        const int32_t down = Math::EvaluateAngleOfPoint(-1, 0);
        Check(up > 16370 && up < 16390 && down < -16370 && down > -16390,
            "source point polynomial retains signed quarter-turn orientation");
        uint32_t seed = 0x414e474c;
        constexpr uint32_t edges[] = {0, 0x80000000, 1, 0x807f, 0x3f800000, 0xbf800000,
            0x3f7fffff, 0x3f800001, 0x7f7fffff, 0xff7fffff, 0x7fc12345, 0xff800000};
        for (unsigned trial = 0; trial < 512; ++trial) {
            const float y = Float(trial % 2 ? Next(seed) : edges[(trial / 2) % 12]);
            const float x = Float(trial % 3 ? Next(seed) : edges[(trial / 3) % 12]);
            const uint32_t unit = trial % 5 < 3 ? trial % 5 : Next(seed);
            Oracle::Memory memory;
            CpuContext expected{};
            expected.cpuRegs.GPR.r[4].UL[0] = 0x100;
            expected.cpuRegs.GPR.r[5].UD[0] = unit;
            expected.fpuRegs.fpr[12].f = y;
            const int32_t converted = Math::EvaluateAngleFrom(y, unit);
            Oracle::Invoke(Math::kAngleFromAddress, expected, memory);
            Check(static_cast<uint32_t>(converted) == memory.Read32(0x100),
                "typed angle-unit conversion matches actual NEW");
            expected = {};
            expected.cpuRegs.GPR.r[4].UL[0] = 0x100;
            expected.fpuRegs.fpr[12].f = y;
            expected.fpuRegs.fpr[13].f = x;
            const int32_t angle = Math::EvaluateAngleOfPoint(y, x);
            Oracle::Invoke(Math::kAngleOfPointAddress, expected, memory);
            Check(static_cast<uint32_t>(angle) == memory.Read32(0x100),
                "typed point angle preserves actual NEW polynomial and two quantizations");
        }
        Check(std::fegetround() == mode, "angle conversion restores caller rounding");
    }
}

void EulerPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "Euler set mode");
        Check(Math::EvaluateEulerAngles(title::MathLibrary::EvaluateIdentity()) ==
            std::array<int32_t, 3>{0, 0, 0}, "identity matrix has zero source Euler angles");
        for (const int32_t yaw : {-8192, 8192}) {
            const auto angles = Math::EvaluateEulerAngles(Math::EvaluateAxisMatrix(yaw, Math::Axis::Yaw));
            Check(angles[0] == 0 && angles[2] == 0 && std::abs(angles[1] - yaw) < 12,
                "pure yaw keeps sign and leaves pitch/roll zero");
        }
        Oracle::Memory memory, expected_memory;
        CpuContext context{};
        context.cpuRegs.GPR.r[4].UL[0] = 0x100;
        context.cpuRegs.GPR.r[5].UL[0] = 0x750;
        context.cpuRegs.GPR.r[6].UL[0] = 0x770;
        context.cpuRegs.GPR.r[7].UL[0] = 0x760;
        context.cpuRegs.GPR.r[29].UL[0] = 0x780;
        for (const unsigned reg : {16u, 17u, 18u, 31u})
            context.cpuRegs.GPR.r[reg].UD[0] = 0xfedcba9876543210ull + reg;
        context.fpuRegs.fpr[20].UL = 0x7fc12345;
        const auto identity = title::MathLibrary::EvaluateIdentity();
        std::memcpy(memory.Translate(0x100, 64), identity.data(), 64);
        CpuContext expected = context;
        expected_memory.bytes = memory.bytes;
        Math math(memory);
        math.EulerAnglesOfMatrix(context);
        Oracle::Invoke(Math::kEulerAnglesAddress, expected, expected_memory);
        Check(context.fpuRegs.fpr[20].UL == 0 && memory.Read32(0x770) == 0,
            "Euler FPR20 restores actual output-aliased guest stack, not native saved input");
        for (const unsigned reg : {16u, 17u, 18u, 31u})
            Check(context.cpuRegs.GPR.r[reg].UD[0] == 0xfedcba9876543210ull + reg,
                "Euler integer registers restore native backups despite aliased save slots");
        Check(std::memcmp(&context, &expected, sizeof(context)) == 0 && memory.bytes == expected_memory.bytes,
            "Euler explicit save/output alias matches complete actual NEW");
        uint32_t seed = 0x45554c52;
        for (unsigned trial = 0; trial < 512; ++trial) {
            Math::Matrix matrix;
            for (auto& row : matrix) for (auto& lane : row) lane = Float(Next(seed));
            if (trial % 2 == 0)
                matrix = Math::EvaluateMatrixXYZ(static_cast<int32_t>(Next(seed)) / 65536,
                    static_cast<int32_t>(Next(seed)) / 65536, static_cast<int32_t>(Next(seed)) / 65536);
            if (trial % 8 >= 3 && trial % 8 <= 5) {
                matrix = title::MathLibrary::EvaluateIdentity();
                matrix[0][1] = Float(0x3a03126eu + trial % 8 - 3);
            }
            if (trial % 8 >= 6) {
                matrix = title::MathLibrary::EvaluateIdentity();
                matrix[1][0] = 0.5f;
                matrix[0][2] = trial % 8 == 6 ? 1 : -1;
            }
            CpuContext actual_source{};
            actual_source.cpuRegs.GPR.r[4].UL[0] = 0x100;
            actual_source.cpuRegs.GPR.r[5].UL[0] = 0x300;
            actual_source.cpuRegs.GPR.r[6].UL[0] = 0x310;
            actual_source.cpuRegs.GPR.r[7].UL[0] = 0x320;
            actual_source.cpuRegs.GPR.r[29].UL[0] = 0x780;
            std::memcpy(memory.Translate(0x100, 64), matrix.data(), 64);
            const auto angles = Math::EvaluateEulerAngles(matrix);
            Oracle::Invoke(Math::kEulerAnglesAddress, actual_source, memory);
            for (unsigned axis = 0; axis < 3; ++axis)
                Check(static_cast<uint32_t>(angles[axis]) == memory.Read32(0x300 + axis * 16),
                    "typed Euler extraction matches actual NEW thresholds/polynomial/gimbal branch");
        }
        Check(std::fegetround() == mode, "Euler restores caller rounding");
    }
}

void SnapPins() {
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "yaw snap set mode");
        const float step = Float(0x3f490fdb);
        const Math::Vector identity{0, 0, 0, 1};
        Check(Math::EvaluateSnapToYaw(identity, step) == identity, "yaw snap leaves identity");
        const auto negative = Math::EvaluateRotation(-8192, Math::Axis::Yaw);
        Check(Math::EvaluateSnapToYaw(negative, step) == identity,
            "source negative half-step bias truncates toward zero instead of rounding symmetrically");
        Check(Math::EvaluateSnapToYaw(negative, 0) == identity,
            "zero step retains source divide/saturate/multiply path to identity");
        uint32_t seed = 0x534e4150;
        constexpr uint32_t steps[] = {0, 0x80000000, 0x3f490fdb, 0xbf490fdb, 0x3f000000,
            0xbe99999a, 1, 0x807f, 0x7f800000, 0xff800000, 0x7fc12345, 0x7f7fffff};
        for (unsigned trial = 0; trial < 512; ++trial) {
            Math::Vector rotation;
            for (auto& lane : rotation) lane = Float(Next(seed));
            if (trial % 2 == 0)
                rotation = Math::EvaluateRotationXYZ(static_cast<int32_t>(Next(seed)) / 65536,
                    static_cast<int32_t>(Next(seed)) / 65536, static_cast<int32_t>(Next(seed)) / 65536);
            if (trial % 16 == 0) rotation = identity;
            const float amount = Float(trial % 3 ? steps[trial % 12] : Next(seed));
            Oracle::Memory memory;
            CpuContext expected{};
            expected.cpuRegs.GPR.r[4].UL[0] = 0x100;
            expected.cpuRegs.GPR.r[29].UL[0] = 0x900;
            expected.fpuRegs.fpr[12].f = amount;
            expected.vuRegs.VF[0].w = 1;
            std::memcpy(memory.Translate(0x100, 16), rotation.data(), 16);
            const auto snapped = Math::EvaluateSnapToYaw(rotation, amount);
            Oracle::Invoke(Math::kSnapYawAddress, expected, memory);
            Check(std::memcmp(snapped.data(), memory.Translate(0x100, 16), 16) == 0,
                "typed yaw snap matches actual NEW matrix/Euler/biased-step closure");
        }
        Check(std::fegetround() == mode, "yaw snap restores caller rounding");
    }
}

void InstallerPins() {
    Oracle::Memory memory;
    CpuContext context{};
    const auto& entries = Oracle::Entries();
    for (const bool selected : {false, true}) {
    for (unsigned failure = 0; failure < entries.size(); ++failure) {
        for (const bool throwing : {false, true}) {
            std::map<uint32_t, Math::Body> table;
            unsigned delegated = 0, published = 0;
            bool rejected = false;
            {
                Math math(memory);
                bool caught = false, result = false;
                try {
                    result = math.Install([&](uint32_t) -> Math::Body {
                        return [&](CpuContext&, uint32_t) { ++delegated; };
                    }, [&](uint32_t address, Math::Body body) {
                        // Reject all restore requests too. Published wrappers
                        // must remain safe after their owner has been destroyed.
                        if (rejected) return false;
                        table[address] = std::move(body);
                        table.at(address)(context, address);
                        if (published++ != failure) return true;
                        rejected = true;
                        if (throwing) throw std::runtime_error("installer rejection");
                        return false;
                    }, selected);
                } catch (const std::runtime_error&) { caught = true; }
                Check(!result && caught == throwing, "failed batch status");
                Check(math.Counts().entries == std::array<uint64_t, Math::kEntryCount>{}, "batch cannot activate early");
            }
            const unsigned before = delegated;
            for (const auto& item : table) item.second(context, item.first);
            Check(delegated == before + table.size(), "failed wrapper owns original after owner destruction");
        }
    }
    }
    unsigned delegated = 0;
    uint32_t received = 0;
    std::map<uint32_t, Math::Body> table;
    Math math(memory);
    Check(math.Install([&](uint32_t) -> Math::Body {
        return [&](CpuContext&, uint32_t address) { ++delegated; received = address; };
    }, [&](uint32_t address, Math::Body body) { table[address] = std::move(body); return true; }), "install batch");
    for (const auto& entry : entries) {
        table.at(entry.address)(context, entry.address + 4);
        Check(received == entry.address + 4, "interior address forwarded without prologue");
    }
    Check(delegated == entries.size() && math.Counts().generated_continuations == entries.size(), "interiors counted");
    Check(math.Counts().entries == std::array<uint64_t, Math::kEntryCount>{}, "interiors do not count as replacements");
    for (const auto count : math.Counts().generated_body_calls) Check(count == 1, "actual interior body counted");
    constexpr uint32_t matrix_interiors[][4] = {
        {0x18bafc, 0x18bb04, 0x18bb0c, 0x18bbb4},
        {0x18bbd4, 0x18bbdc, 0x18bbe4, 0x18bc8c},
        {0x18bcac, 0x18bcb4, 0x18bcbc, 0x18bd64}};
    for (unsigned axis = 0; axis < 3; ++axis) {
        for (const auto interior : matrix_interiors[axis]) {
            table.at(entries[9 + axis].address)(context, interior);
            Check(received == interior, "real matrix interior never replays the complete prologue");
        }
        Check(math.Counts().generated_body_calls[9 + axis] == 5, "real matrix interiors count actual originals");
    }
    for (const uint32_t interior : {0x186794, 0x1867ac, 0x1867b4, 0x1867bc, 0x1867d4,
                                   0x1867dc, 0x1867e4, 0x1867f4, 0x1867fc, 0x1868f4}) {
        table.at(Math::kRotationXYZAddress)(context, interior);
        Check(received == interior, "real XYZ interior never replays prologue or uses absent native return");
    }
    Check(math.Counts().generated_body_calls[12] == 11, "XYZ interior actual bodies counted");
    for (const uint32_t interior : {0x18b9bc, 0x18b9d4, 0x18b9e8, 0x18b9f4}) {
        table.at(Math::kTangentAddress)(context, interior);
        Check(received == interior, "real tangent interior forwarded without entry scratch evaluation");
    }
    Check(math.Counts().generated_body_calls[13] == 5, "tangent interior actual bodies counted");
    constexpr uint32_t pair_interiors[][9] = {
        {0x182ae4, 0x182aec, 0x182af4, 0x182afc, 0x182ba4, 0x182bac, 0x182c54, 0x182d30, 0x182d34},
        {0x182d54, 0x182d5c, 0x182d64, 0x182d6c, 0x182e14, 0x182e1c, 0x182ec4, 0x182fa0, 0x182fa4},
        {0x182fc4, 0x182fcc, 0x182fd4, 0x182fdc, 0x183084, 0x18308c, 0x183134, 0x183210, 0x183214}};
    for (unsigned pair = 0; pair < 3; ++pair) {
        for (const auto interior : pair_interiors[pair]) {
            table.at(entries[14 + pair].address)(context, interior);
            Check(received == interior, "real paired matrix interior preserves original dispatch");
        }
        Check(math.Counts().generated_body_calls[14 + pair] == 10, "paired matrix interior bodies counted");
    }
    for (const uint32_t interior : {0x183234, 0x183250, 0x183258, 0x183260, 0x183274,
                                   0x18327c, 0x183284, 0x183298, 0x1832a0, 0x1833c4}) {
        table.at(Math::kMatrixXYZAddress)(context, interior);
        Check(received == interior, "real XYZ matrix interior preserves original dispatch");
    }
    Check(math.Counts().generated_body_calls[17] == 11, "XYZ matrix actual interior bodies counted");
    for (const uint32_t interior : {0x188350, 0x188354, 0x1883b4, 0x1883c8, 0x1883f8, 0x188508, 0x18850c}) {
        table.at(Math::kTurnTowardAddress)(context, interior);
        Check(received == interior, "real direction-turn interior preserves original dispatch");
    }
    Check(math.Counts().generated_body_calls[18] == 8, "direction-turn actual interior bodies counted");
    for (const uint32_t interior : {0x1881d8, 0x188208, 0x188228, 0x188234, 0x188240, 0x188258, 0x1882f4}) {
        table.at(Math::kTurnSineCosineAddress)(context, interior);
        Check(received == interior, "real Rodrigues interior delegates without repeating normalization");
    }
    Check(math.Counts().generated_body_calls[19] == 8, "Rodrigues actual interior bodies counted");
    for (const uint32_t interior : {0x18de14, 0x18de60, 0x18de64}) {
        table.at(Math::kTurnAxisAddress)(context, interior);
        Check(received == interior, "real axis-turn interior delegates without absent native backup");
    }
    Check(math.Counts().generated_body_calls[20] == 4, "axis-turn actual interior bodies counted");
    for (const uint32_t interior : {0x18d820, 0x18d85c, 0x18d884, 0x18d890}) {
        table.at(Math::kRotationAxisSineAddress)(context, interior);
        Check(received == interior, "real axis-quaternion leaf interior preserves original dispatch");
    }
    Check(math.Counts().generated_body_calls[21] == 5, "axis-quaternion leaf interior bodies counted");
    table.at(Math::kRotationAxisAddress)(context, 0x18d808);
    Check(received == 0x18d808, "axis-quaternion return interior never uses an absent native backup");
    Check(math.Counts().generated_body_calls[22] == 2, "axis-quaternion return body counted");
    for (const uint32_t interior : {0x185070, 0x1850a0, 0x1850c0, 0x1850cc, 0x1850d8}) {
        table.at(Math::kMatrixAxisSineAddress)(context, interior);
        Check(received == interior, "real arbitrary-axis matrix leaf interior delegates");
    }
    Check(math.Counts().generated_body_calls[23] == 6, "arbitrary-axis leaf interior bodies counted");
    for (const uint32_t interior : {0x18cebc, 0x18cec4, 0x18cecc, 0x18cf18, 0x18cf1c}) {
        table.at(Math::kMatrixAxisAddress)(context, interior);
        Check(received == interior, "real axis-angle caller interior preserves native-backup guard");
    }
    Check(math.Counts().generated_body_calls[24] == 6, "axis-angle caller interior bodies counted");
    for (const uint32_t interior : {0x11c0f8, 0x11c10c}) {
        table.at(Math::kInverseLength4Address)(context, interior);
        Check(received == interior, "real inverse-length interior delegates");
    }
    Check(math.Counts().generated_body_calls[25] == 3, "inverse-length actual interior bodies counted");
    for (const uint32_t interior : {0x187164, 0x18716c, 0x187194, 0x1871ac, 0x18720c, 0x187220,
                                   0x187294, 0x1872d8, 0x18731c, 0x187380, 0x187390, 0x1873b0,
                                   0x1873c0, 0x1873e8, 0x18744c, 0x187480, 0x18749c}) {
        table.at(Math::kSlerpAddress)(context, interior);
        Check(received == interior, "real slerp interior never restores absent native saved registers");
    }
    Check(math.Counts().generated_body_calls[26] == 18, "slerp actual interior bodies counted");
    constexpr uint32_t angle_interiors[][6] = {
        {0x11bfac, 0x11bfb4, 0x11bfbc, 0x11bfd0, 0x11bfe0, 0x11bff4},
        {0x182948, 0x182970, 0x1829d4, 0x182a5c, 0x182a70, 0x182abc}};
    for (unsigned operation = 0; operation < 2; ++operation) {
        for (const auto interior : angle_interiors[operation]) {
            table.at(entries[27 + operation].address)(context, interior);
            Check(received == interior, "real source angle interior delegates with original live state");
        }
        Check(math.Counts().generated_body_calls[27 + operation] == 7,
            "angle interior actual body execution counted");
    }
    for (const uint32_t interior : {0x1851cc, 0x1851d0, 0x1851dc, 0x1851f4, 0x1851f8, 0x185204,
             0x18521c, 0x185220, 0x18522c, 0x185244, 0x185248, 0x185254, 0x18527c, 0x185280,
             0x18528c, 0x1852ac, 0x1852b0, 0x1852bc, 0x1852d8, 0x1852dc, 0x1852e8, 0x1852f0,
             0x185300, 0x185310, 0x185314, 0x185338, 0x1853a8, 0x1853ac, 0x1853d4, 0x1853e4,
             0x1853f0, 0x185400, 0x185408, 0x18541c, 0x18542c, 0x185434, 0x185444, 0x185460,
             0x185464}) {
        table.at(Math::kEulerAnglesAddress)(context, interior);
        Check(received == interior, "real Euler interior delegates without replaying save/restore");
    }
    Check(math.Counts().generated_body_calls[29] == 40, "Euler actual interior original entries counted");
    for (const uint32_t interior : {0x1874e4, 0x187504, 0x187550, 0x18755c, 0x187578, 0x1875c0}) {
        table.at(Math::kSnapYawAddress)(context, interior);
        Check(received == interior, "real yaw-snap interior delegates without absent native saves");
    }
    Check(math.Counts().generated_body_calls[30] == 7, "yaw-snap actual interior original entries counted");
    for (const bool selected : {false, true}) {
        Math throwing(memory);
        Check(throwing.Install([&](uint32_t address) -> Math::Body {
            return [&, address](CpuContext&, uint32_t) {
                size_t index = 0;
                while (entries[index].address != address) ++index;
                Check(throwing.Counts().generated_body_calls[index] == 1, "counter precedes original body entry");
                throw std::runtime_error("original entered");
            };
        }, [&](uint32_t address, Math::Body body) { table[address] = std::move(body); return true; }, selected),
            "selection-aware install");
        for (const auto& entry : entries) {
            bool caught = false;
            try { table.at(entry.address)(context, selected ? entry.address + 4 : 0); }
            catch (const std::runtime_error&) { caught = true; }
            Check(caught, "original exception propagated");
        }
        Check(throwing.Counts().entries == std::array<uint64_t, Math::kEntryCount>{}, "delegation is not replacement");
        Check(throwing.Census().find("generated_body_rotation_yz=1") != std::string::npos, "actual census field");
    }
}

void Seed(Oracle::Memory& memory, CpuContext& context, uint32_t& seed, unsigned trial, unsigned operation) {
    for (auto& byte : memory.bytes) byte = static_cast<uint8_t>(Next(seed));
    for (size_t i = 0; i < sizeof(context); ++i)
        reinterpret_cast<uint8_t*>(&context)[i] = static_cast<uint8_t>(Next(seed));
    constexpr uint32_t mirrors[] = {0, 0x80000000u, 0xa0000000u, 0x70000000u};
    constexpr uint32_t angles[] = {0, 1, 0xffffffffu, 32767, 32768, 0xffff8000u,
        65535, 65536, 0xffff0000u, 0x7fffffffu, 0x80000000u};
    const uint32_t prefix = mirrors[(trial / 16) % 4];
    const auto address = [&](uint32_t offset) { return prefix | offset | (trial % 16); };
    auto& r = context.cpuRegs.GPR.r;
    const uint32_t input = address(0x100);
    const uint32_t output = address(trial % 3 == 0 ? 0x100 : trial % 3 == 1 ? 0x700 : 0x300);
    const uint32_t angle = trial % 2 == 0 ? angles[(trial / 2) % 11] : Next(seed);
    r[29].UL[0] = address(0x710);
    // Source zero-register behavior is retained even for noncanonical saved
    // contexts; upper words and unrelated VF/ACC/I/Q/flags remain arbitrary.
    if (trial % 2 == 0) r[0].UD[0] = 0;
    if (operation == 0) {
        r[4].UL[0] = input;
        r[5].UL[0] = output;
        r[6].UL[0] = trial % 4 ? address(0x500) : output;
        memory.Write32(input, angle);
    } else if (operation == 1) {
        r[4].UL[0] = angle;
        r[5].UL[0] = output;
    } else if (operation == 2) {
        r[4].UL[0] = angle;
        r[5].UL[0] = Next(seed);
        r[6].UL[0] = output;
    } else if (operation < 6) {
        r[4].UL[0] = output;
        r[5].UL[0] = input;
        memory.Write32(input, angle);
    } else if (operation < 9) {
        r[29].UL[0] = address(0x730);
        r[4].UL[0] = output;
        r[5].UL[0] = input;
        r[6].UL[0] = address(trial % 7 == 0 ? 0x100 : 0x200);
        // Ensure both-zero, first-zero, second-zero and both-nonzero paths,
        // with signed boundary/random angles and input/output/scratch alias.
        memory.Write32(input, trial % 4 <= 1 ? 0 : angle);
        memory.Write32(r[6].UL[0], trial % 4 == 0 || trial % 4 == 2 ? 0 : Next(seed));
    } else if (operation < 12) {
        r[29].UL[0] = address(0x730);
        r[4].UL[0] = trial % 4 == 0 ? address(0x710) : trial % 4 == 1 ? address(0x720) : output;
        r[5].UL[0] = trial % 5 == 0 ? address(0x720) : input;
        // Include zero/nonzero paths and output overlap with trig scratch,
        // native return backup, or the angle itself. The prologue may change
        // an angle aliased with its saved return before the source reads it.
        memory.Write32(r[5].UL[0], trial % 3 == 0 ? 0 : angle);
    } else if (operation == 12) {
        r[29].UL[0] = address(0x780);
        // Canonical source r0, plus a backed noncanonical stack displacement.
        // Preserve both r0 comparison and pointer arithmetic, not just zeros.
        r[0].UD[0] = trial % 17 == 0 ? 16 : 0;
        r[4].UL[0] = trial % 4 == 0 ? address(0x730) :
            trial % 4 == 1 ? address(0x770) : output;
        r[5].UL[0] = trial % 11 == 0 ? address(0x770) : input;
        r[6].UL[0] = address(trial % 7 == 0 ? 0x100 : 0x200);
        r[7].UL[0] = address(trial % 13 == 0 ? 0x730 : 0x400);
        // All eight zero/nonzero combinations, signed extrema, odd pointers,
        // angle aliasing and full/nested scratch or saved-return output aliases.
        memory.Write32(r[5].UL[0], trial & 1 ? angle : 0);
        memory.Write32(r[6].UL[0], trial & 2 ? Next(seed) : r[0].UL[0]);
        memory.Write32(r[7].UL[0], trial & 4 ? Next(seed) : 0);
    } else if (operation == 13) {
        r[4].UL[0] = trial % 3 == 0 ? address(0x700) : input;
        memory.Write32(r[4].UL[0], angle);
    } else if (operation < 17) {
        r[29].UL[0] = address(0x780);
        r[4].UL[0] = trial % 5 < 4 ? address(0x740 + (trial % 5) * 16) : output;
        r[5].UL[0] = trial % 11 == 0 ? address(0x770) : input;
        r[6].UL[0] = address(trial % 7 == 0 ? 0x100 : trial % 13 == 0 ? 0x770 : 0x200);
        memory.Write32(r[5].UL[0], trial & 1 ? angle : 0);
        memory.Write32(r[6].UL[0], trial & 2 ? Next(seed) : 0);
    } else if (operation == 17) {
        r[29].UL[0] = address(0x780);
        r[0].UD[0] = trial % 17 == 0 ? 16 : 0;
        r[4].UL[0] = trial % 5 < 4 ? address(0x730 + (trial % 5) * 16) : output;
        r[5].UL[0] = trial % 11 == 0 ? address(0x770) : input;
        r[6].UL[0] = address(trial % 7 == 0 ? 0x100 : 0x200);
        r[7].UL[0] = address(trial % 13 == 0 ? 0x730 : 0x400);
        // x compares to raw r0; y and z compare to literal zero in this ABI.
        memory.Write32(r[5].UL[0], trial & 1 ? angle : r[0].UL[0]);
        memory.Write32(r[6].UL[0], trial & 2 ? Next(seed) : 0);
        memory.Write32(r[7].UL[0], trial & 4 ? Next(seed) : 0);
    } else if (operation == 18) {
        r[29].UL[0] = address(0x760);
        const uint32_t destinations[] = {0x100, 0x200, 0x730, 0x740, 0x750, 0x300};
        r[4].UL[0] = address(destinations[trial % 6]);
        r[5].UL[0] = input;
        r[6].UL[0] = address(trial % 3 == 0 ? 0x100 : trial % 3 == 1 ? 0x200 : 0x730);
        constexpr uint32_t shares[] = {0, 0x80000000, 0x3f000000, 0x3f800000, 0xbf800000,
            0x40000000, 0x3eaaaaab, 0x7f7fffff, 0x7f800000, 0x7fc12345, 1, 0xff800000};
        context.fpuRegs.fpr[12].UL = trial % 2 ? Next(seed) : shares[(trial / 2) % 12];
        if (trial % 4 == 0) {
            const Math::Vector first{1, 0, 0, Float(0x0000807f)};
            const Math::Vector targets[] = {{1, 0, 0, Float(0x8000807f)}, {0, 1, 0, -0.0f},
                {-1, 0, 0, Float(0x7fc12345)}, {0, 0, 0, Float(0x00000001)}};
            std::memcpy(memory.Translate(r[5].UL[0], 16), first.data(), 16);
            std::memcpy(memory.Translate(r[6].UL[0], 16), targets[(trial / 4) % 4].data(), 16);
        }
    } else if (operation < 25) {
        r[29].UL[0] = address(0x760);
        r[4].UL[0] = address(trial % 4 == 0 ? 0x740 : trial % 4 == 1 ? 0x750 : 0x100);
        if (operation == 22 || operation == 24) {
            const uint32_t destinations[] = {0x100, 0x200, 0x730, 0x740, 0x750, 0x760};
            r[4].UL[0] = address(destinations[trial % 6]);
        }
        r[5].UL[0] = trial % 5 == 0 ? r[4].UL[0] : trial % 5 == 1 ? r[4].UL[0] + 4 :
            address(trial % 5 == 2 ? 0x740 : 0x200);
        const uint64_t unit_flags[] = {0, 1, 0x100000000ull, 0xffffffffffffffffull};
        if (trial % 4 == 0) {
            const Math::Vector axes[] = {{0, 0, 0, Float(0x807f)}, {0, 0, 2, -0.0f},
                {1e-7f, 0, 0, Float(1)}, {0, 0, 1e-3f, Float(0x7fc12345)}};
            std::memcpy(memory.Translate(r[5].UL[0], 16), axes[(trial / 4) % 4].data(), 16);
        }
        if (operation == 19 || operation == 21 || operation == 23) {
            r[6].UD[0] = unit_flags[(trial / 4) % 4];
            context.fpuRegs.fpr[12].UL = trial % 2 ? Next(seed) : 0x3f800000u;
            context.fpuRegs.fpr[13].UL = trial % 2 ? Next(seed) : 0;
        } else {
            r[6].UL[0] = trial % 11 == 0 ? address(0x750) :
                trial % 7 == 0 ? r[4].UL[0] : address(0x400);
            r[7].UD[0] = unit_flags[(trial / 4) % 4];
            r[0].UD[0] = trial % 17 == 0 ? 16 : trial % 13 == 0 ? 0xffffffff80000000ull : 0;
            memory.Write32(r[6].UL[0], trial % 3 == 0 ? r[0].UL[0] : angle);
        }
    } else if (operation == 25) {
        r[4].UL[0] = trial % 3 == 0 ? address(0x740) : input;
        constexpr uint32_t epsilons[] = {0, 0x2edbe6ff, 0x3f800000, 0x40a00000, 0x7fc12345, 0x7f800000};
        context.fpuRegs.fpr[12].UL = trial % 2 ? Next(seed) : 0x0000807fu;
        context.fpuRegs.fpr[13].UL = trial % 2 ? Next(seed) : epsilons[(trial / 2) % 6];
        if (trial % 4 == 0) {
            const Math::Vector vectors[] = {{}, {0, 0, 0, 5}, {3, 0, 0, 4},
                {Float(0x2edbe6ff), 0, 0, 0}};
            std::memcpy(memory.Translate(r[4].UL[0], 16), vectors[(trial / 4) % 4].data(), 16);
        }
    } else if (operation == 26) {
        r[29].UL[0] = address(0x780);
        r[0].UD[0] = trial % 17 == 0 ? 16 : 0;
        const uint32_t destinations[] = {0x100, 0x200, 0x760, 0x770, 0x780, 0x300};
        r[4].UL[0] = address(destinations[trial % 6]);
        r[5].UL[0] = trial % 7 == 0 ? address(0x770) : input;
        r[6].UL[0] = trial % 5 == 0 ? r[4].UL[0] :
            address(trial % 3 == 0 ? 0x100 : trial % 3 == 1 ? 0x200 : 0x760);
        constexpr uint32_t shares[] = {0xbf800000, 0, 0x0000807f, 0x3e800000, 0x3f000000,
            0x3f800000, 0x40000000, 0x7fc12345, 0x7f800000, 0xff800000};
        context.fpuRegs.fpr[12].UL = trial % 2 ? Next(seed) : shares[(trial / 2) % 10];
        if (trial % 4 == 0) {
            const Math::Vector first{0, 0, 0, 1};
            const Math::Vector targets[] = {{0, 0, 0, 1}, {0, 0, 0, -1},
                {0, 0.70710678f, 0, 0.70710678f}, {0, 0.1f, 0, 0.995f}};
            std::memcpy(memory.Translate(r[5].UL[0], 16), first.data(), 16);
            std::memcpy(memory.Translate(r[6].UL[0], 16), targets[(trial / 4) % 4].data(), 16);
        }
    } else if (operation < 29) {
        r[4].UL[0] = output;
        const uint64_t units[] = {0, 1, 2, 0x100000001ull, 0xffffffffffffffffull};
        r[5].UD[0] = trial % 3 ? units[(trial / 3) % 5] : Next(seed);
        if (trial % 7 == 0) {
            r[0].UD[0] = 0xffffffff80000000ull;
            r[5].SD[0] = static_cast<int32_t>(r[0].UL[0] + 1 + trial % 2);
        }
        constexpr uint32_t points[][2] = {{0, 0}, {0, 0xbf800000}, {0x3f800000, 0},
            {0xbf800000, 0}, {0x3f800000, 0x3f800000}, {0x3f800000, 0xbf800000},
            {0x3f800000, 0x80000001}, {0x807f, 0x8000807f},
            {0x7f800000, 0xff800000}, {0x7fc12345, 0x3f800000}};
        if (trial % 2 == 0) {
            context.fpuRegs.fpr[12].UL = points[(trial / 2) % 10][0];
            context.fpuRegs.fpr[13].UL = points[(trial / 2) % 10][1];
        }
    } else if (operation == 29) {
        r[29].UL[0] = address(0x780);
        r[0].UD[0] = trial % 17 == 0 ? 16 : 0;
        r[4].UL[0] = trial % 11 == 0 ? address(0x740) : input;
        const uint32_t destinations[] = {0x100, 0x104, 0x120, 0x740, 0x750, 0x758,
            0x760, 0x768, 0x770, 0x300, 0x320};
        r[5].UL[0] = address(destinations[trial % 11]);
        r[6].UL[0] = trial % 5 == 0 ? r[5].UL[0] : address(destinations[(trial + 3) % 11]);
        r[7].UL[0] = trial % 7 == 0 ? r[6].UL[0] : address(destinations[(trial + 7) % 11]);
        if (trial % 4 == 0) {
            auto matrix = title::MathLibrary::EvaluateIdentity();
            const unsigned shape = (trial / 4) % 8;
            if (shape == 1) matrix = Math::EvaluateAxisMatrix(8192, Math::Axis::Yaw);
            if (shape == 2) matrix = Math::EvaluateMatrixXYZ(1000, 4096, -2000);
            if (shape >= 3 && shape <= 5) matrix[0][1] = Float(0x3a03126e + shape - 3);
            if (shape >= 6) { matrix[0][2] = shape == 6 ? 1 : -1; matrix[1][0] = 0.5f; }
            std::memcpy(memory.Translate(r[4].UL[0], 64), matrix.data(), 64);
        }
    } else {
        r[29].UL[0] = address(0x900);
        r[0].UD[0] = trial % 17 == 0 ? 16 : 0;
        const uint32_t rotations[] = {0x100, 0x840, 0x850, 0x860, 0x870, 0x880, 0x890,
            0x8a0, 0x8b0, 0x8c0, 0x8d0, 0x8d8, 0x8e0, 0x8e8, 0x8f0};
        r[4].UL[0] = address(rotations[trial % 15]);
        constexpr uint32_t steps[] = {0, 0x80000000, 0x3f490fdb, 0xbf490fdb, 0x3f000000,
            0xbe99999a, 1, 0x807f, 0x7f800000, 0xff800000, 0x7fc12345, 0x7f7fffff};
        context.fpuRegs.fpr[12].UL = trial % 2 ? Next(seed) : steps[(trial / 2) % 12];
        if (trial % 4 == 0) {
            const Math::Vector values[] = {{0, 0, 0, 1}, Math::EvaluateRotation(-8192, Math::Axis::Yaw),
                Math::EvaluateRotation(24576, Math::Axis::Yaw), Math::EvaluateRotationXYZ(4096, -10000, 7000)};
            std::memcpy(memory.Translate(r[4].UL[0], 16), values[(trial / 4) % 4].data(), 16);
            context.vuRegs.VF[0].x = 0; context.vuRegs.VF[0].y = 0;
            context.vuRegs.VF[0].z = 0; context.vuRegs.VF[0].w = 1;
        }
    }
}

bool Compare(unsigned operation, unsigned trial, uint32_t& seed, int mode, bool selected,
             MathLibraryOracleArtifacts& artifacts) {
    Oracle::Memory memory, expected_memory;
    CpuContext actual, expected;
    Seed(memory, actual, seed, trial, operation);
    std::memcpy(&expected, &actual, sizeof(actual));
    expected_memory.bytes = memory.bytes;
    Math math(memory);
    unsigned originals = 0;
    std::map<uint32_t, Math::Body> table;
    if (!math.Install([&](uint32_t address) -> Math::Body {
        return [&, address](CpuContext& context, uint32_t) {
            ++originals;
            Oracle::Invoke(address, context, memory, [&](uint32_t nested, CpuContext& nested_context) {
                table.at(nested)(nested_context, nested);
            });
        };
    }, [&](uint32_t address, Math::Body body) { table[address] = std::move(body); return true; }, selected)) return false;
    const auto& entry = Oracle::Entries()[operation];
    const uint32_t dispatch = trial % 2 ? entry.address : 0;
    artifacts.Begin({entry.address, dispatch, operation, trial, 0, selected},
        expected, expected_memory.bytes.data());
    const uint64_t before_reference = Oracle::Model::Calls();
    const auto reference_bodies_before = Oracle::BodyCalls();
    artifacts.RunModel([&] { Oracle::Invoke(entry.address, expected, expected_memory); });
    const auto reference_bodies_after = Oracle::BodyCalls();
    const uint64_t before = Oracle::Model::Calls();
    const uint64_t reference_model_calls = before - before_reference;
    artifacts.ModelOutput(expected, expected_memory.bytes.data());
    artifacts.CheckActualInput(actual, memory.bytes.data());
    table.at(entry.address)(actual, dispatch);
    const auto actual_bodies_after = Oracle::BodyCalls();
    if (artifacts.Enabled()) artifacts.ActualOutput(actual, memory.bytes.data(), math.Census());
    const auto counts = math.Counts();
    uint64_t replacements = 0;
    for (const auto count : counts.entries) replacements += count;
    uint64_t generated = 0;
    for (const auto count : counts.generated_body_calls) generated += count;
    uint64_t closure_entries = 0;
    bool entry_counts_match = true;
    for (size_t i = 0; i < Math::kEntryCount; ++i) {
        const auto reference = reference_bodies_after[i] - reference_bodies_before[i];
        closure_entries += reference;
        const auto actual_body = actual_bodies_after[i] - reference_bodies_after[i];
        entry_counts_match &= counts.entries[i] == (selected ? reference : 0) &&
            counts.generated_body_calls[i] == (selected ? 0 : reference) &&
            actual_body == counts.generated_body_calls[i];
    }
    const auto expected_originals = selected ? 0 : closure_entries;
    selected_entries += replacements;
    control_bodies += generated;
    if (memory.bytes == expected_memory.bytes && std::memcmp(&actual, &expected, sizeof(actual)) == 0 &&
        std::fegetround() == mode && replacements == (selected ? closure_entries : 0) &&
        entry_counts_match && originals == expected_originals && generated == expected_originals &&
        counts.generated_continuations == 0 &&
        Oracle::Model::Calls() - before == (selected ? 0 : reference_model_calls)) return true;
    std::fprintf(stderr, "Trig oracle FAIL op=%s trial=%u mode=%d originals=%u\n",
        entry.name, trial, mode, originals);
    for (size_t offset = 0; offset < sizeof(actual); offset += 4) {
        uint32_t a, b;
        std::memcpy(&a, reinterpret_cast<const uint8_t*>(&actual) + offset, 4);
        std::memcpy(&b, reinterpret_cast<const uint8_t*>(&expected) + offset, 4);
        if (a != b) std::fprintf(stderr, " context+%zu actual=%08x expected=%08x\n", offset, a, b);
    }
    for (size_t offset = 0; offset < memory.bytes.size(); ++offset) {
        if (memory.bytes[offset] == expected_memory.bytes[offset]) continue;
        std::fprintf(stderr, " first memory difference +%zu actual=%02x expected=%02x\n",
            offset, static_cast<unsigned>(memory.bytes[offset]), static_cast<unsigned>(expected_memory.bytes[offset]));
        break;
    }
    return false;
}
}  // namespace

void MathLibraryTrigOracle::Model::Execute(CpuContext& context, uint32_t address) {
    // Supply only the production matcher's signature, taken from NEW's
    // public constants. No captured image, recorded result, or substitute math.
    const auto& code = Program();
    VuChopRoundGuard rounding;
    if (!LibVu0SinCos::TryExecute(code.data(), code.size(), context.vuRegs, address)) std::abort();
    ++model_calls;
}

uint64_t MathLibraryTrigOracle::Model::Calls() { return model_calls; }

void MathLibraryTrigOracle::Model::ResumeIfWaiting(CpuContext&) { std::abort(); }

void MathLibraryTrigOracle::UnexpectedCall::Execute(CpuContext&, uint32_t) { std::abort(); }

int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryTrigOracle",
        Oracle::SourceDigest(), Oracle::Memory{}.bytes.size(), 4096);
    if (artifacts.Enabled()) {
        const auto& code = Program();
        artifacts.Fixture("trig.program.bin", code.data(), code.size());
    }
    const int saved = std::fegetround();
    if (saved == -1) return 1;
    ValuePins();
    TangentPins();
    MatrixPairPins();
    MatrixXYZPins();
    TurnTowardPins();
    AxisTurnPins();
    AxisRotationPins();
    AxisMatrixPins();
    InverseLengthPins();
    SlerpPins();
    AnglePins();
    EulerPins();
    SnapPins();
    InstallerPins();
    std::printf("MathLibraryTrigOracle independent pins=%u failures=%u\n", checks, failures);
    std::printf("NEW source sha256=%s\n", Oracle::SourceDigest());
    uint32_t seed = 0x54524947;
    unsigned comparisons = 0;
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        if (std::fesetround(mode) != 0) return 1;
        for (unsigned operation = 0; operation < Oracle::kEntryCount; ++operation) {
            for (unsigned trial = 0; trial < Oracle::kCasesPerEntry; ++trial) {
                uint32_t control_seed = seed;
                if (!Compare(operation, trial, seed, mode, true, artifacts) ||
                    !Compare(operation, trial, control_seed, mode, false, artifacts) || seed != control_seed) {
                    (void)std::fesetround(saved);
                    return 1;
                }
                comparisons += 2;
            }
        }
    }
    if (std::fesetround(saved) != 0) return 1;
    constexpr unsigned per_selection = 4 * Oracle::kCasesPerEntry * Oracle::kEntryCount;
    std::printf("MathLibraryTrigOracle comparisons=%u selected_replacements=%llu model_on_replacement=0 "
                "selected_generated_bodies=0 control_generated_bodies=%llu differences=0\n",
        comparisons, static_cast<unsigned long long>(selected_entries),
        static_cast<unsigned long long>(control_bodies));
    if (failures != 0 || comparisons != 2 * per_selection) return 1;
    artifacts.Finish();
    return 0;
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryTrigOracle: %s\n", error.what());
        return 1;
    }
}
