#include "games/slus_20909_crash_twinsanity/hle/math/joint_math.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "math_library_joint_image.h"

#include <cfenv>
#include <cstdio>
#include <cstring>

namespace {
using Math = title::JointMath;
unsigned checks = 0, failures = 0;
std::array<uint64_t, 5> originals{}, selected{};
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
            trial % 5 == 1 ? Float(word) : static_cast<float>(static_cast<int32_t>(word)) / 536870912.0f;
    }
}
void RandomState(VuUnit& unit, uint32_t& seed) {
    for (auto& row : unit.regs().VF) for (auto& word : row.I) word = Next(seed);
    for (auto& word : unit.regs().ACC.I) word = Next(seed);
    unit.regs().Q = Float(Next(seed));
}
void Report(const float* actual, const float* control, size_t words, unsigned trial, uint32_t entry) {
    for (size_t lane = 0; lane < words; ++lane) {
        uint32_t a, b;
        std::memcpy(&a, actual + lane, 4); std::memcpy(&b, control + lane, 4);
        if (a != b) {
            std::fprintf(stderr, "entry=%x trial=%u lane=%zu selected=%08x control=%08x\n",
                entry, trial, lane, a, b);
            return;
        }
    }
}
bool Basis(unsigned trial, uint32_t& seed, int mode, bool scaled) {
    Math::Vector rotation, parent, own;
    Input(rotation, seed, trial); Input(parent, seed, trial); Input(own, seed, trial);
    if (trial % 7 == 0) rotation = {0, 0, 0, 1};
    if (trial % 7 == 1) rotation = {0, 0.6f, 0, 0.8f};
    if (trial % 37 == 0)
        rotation = {Float(0xbcd903ab), Float(0xbac794af), Float(0x3c151e68), Float(0x3f7fe636)};
    if (trial % 11 == 0) parent = {1, 1, 1, Float(Next(seed))};
    VuUnit original(0);
    original.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    RandomState(original, seed);
    std::memcpy(original.regs().VF[31].F, rotation.data(), 16);
    std::memcpy(original.regs().VF[29].F, parent.data(), 16);
    std::memcpy(original.regs().VF[30].F, own.data(), 16);
    const auto inputs = std::array<Math::Vector, 3>{rotation, parent, own};
    VuInterpreterTier model;
    ++originals[scaled];
    const uint32_t entry = scaled ? 0x668 : 0x548;
    model.Run(original, entry);
    Check(original.regs().TPC == (scaled ? 0x790u : 0x668u), "joint basis reaches reviewed return");
    const auto before = originals;
    ++selected[scaled];
    const auto result = Math::RotationBasis(rotation, parent, scaled ? &own : nullptr);
    const bool same = std::memcmp(result.data(), original.regs().VF[1].F, sizeof(result)) == 0;
    Check(same, "source rotation/scale compensation differs from actual NEW");
    Check(originals == before && std::fegetround() == mode, "selected basis owns no model or rounding state");
    const auto after = std::array<Math::Vector, 3>{rotation, parent, own};
    Check(std::memcmp(inputs.data(), after.data(), sizeof(inputs)) == 0, "raw source basis inputs remain unchanged");
    if (!same) {
        Report(result[0].data(), original.regs().VF[1].F, 12, trial, entry);
        for (size_t input = 0; input < inputs.size(); ++input) {
            std::array<uint32_t, 4> words;
            std::memcpy(words.data(), inputs[input].data(), 16);
            std::fprintf(stderr, "input%zu=%08x,%08x,%08x,%08x\n", input,
                words[0], words[1], words[2], words[3]);
        }
        for (unsigned reg : {9u, 10u, 11u, 12u, 13u, 29u, 30u, 31u}) {
            const auto* words = original.regs().VF[reg].I;
            std::fprintf(stderr, "controlVF%u=%08x,%08x,%08x,%08x\n", reg,
                words[0], words[1], words[2], words[3]);
        }
    }
    return same;
}
bool Parent(unsigned trial, uint32_t& seed, int mode) {
    Math::Matrix matrix, parent;
    for (auto& row : matrix) Input(row, seed, trial);
    for (auto& row : parent) Input(row, seed, trial);
    if (trial % 13 == 0) parent = matrix;
    VuUnit original(0);
    original.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    RandomState(original, seed);
    std::memcpy(original.regs().VF[1].F, matrix.data(), sizeof(matrix));
    std::memcpy(original.regs().VF[5].F, parent.data(), sizeof(parent));
    VuInterpreterTier model;
    ++originals[2];
    model.Run(original, 0x790);
    Check(original.regs().TPC == 0x810, "parent composition reaches reviewed return");
    const auto before = originals;
    ++selected[2];
    const auto inputs = std::array<Math::Matrix, 2>{matrix, parent};
    const auto result = Math::MultiplyByParent(matrix, trial % 13 == 0 ? matrix : parent);
    const bool same = std::memcmp(result.data(), original.regs().VF[1].F, sizeof(result)) == 0;
    Check(same, "source parent composition differs from actual NEW");
    Check(originals == before && std::fegetround() == mode && result[3][3] == 1,
        "parent composition restores rounding, owns no model and publishes affine translation");
    const auto after = std::array<Math::Matrix, 2>{matrix, parent};
    Check(std::memcmp(inputs.data(), after.data(), sizeof(inputs)) == 0,
        "parent composition keeps raw/aliased source matrices unchanged");
    if (!same) Report(result[0].data(), original.regs().VF[1].F, 16, trial, 0x790);
    return same;
}
bool Turn(unsigned trial, uint32_t& seed, int mode) {
    Math::Vector by, rotation;
    Input(by, seed, trial); Input(rotation, seed, trial);
    if (trial % 7 == 0) rotation = {0, 0, 0, 1};
    if (trial % 7 == 1) by = {0, 0.6f, 0, 0.8f};
    if (trial % 37 == 0)
        rotation = {Float(0xbcd903ab), Float(0xbac794af), Float(0x3c151e68), Float(0x3f7fe636)};
    if (trial % 13 == 0) by = rotation;
    VuUnit original(0);
    original.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    RandomState(original, seed);
    std::memcpy(original.regs().VF[1].F, rotation.data(), 16);
    std::memcpy(original.regs().VF[31].F, by.data(), 16);
    const auto inputs = std::array<Math::Vector, 2>{by, rotation};
    VuInterpreterTier model;
    ++originals[3];
    model.Run(original, 0xa70);
    // The end flag sits at aa0; its delay slot aa8 runs and ab0/ab8 are padding before the ray program.
    Check(original.regs().TPC == 0xab0, "joint turn reaches reviewed return");
    const auto before = originals;
    ++selected[3];
    const auto result = Math::Turn(by, rotation);
    const bool same = std::memcmp(result.data(), original.regs().VF[1].F, sizeof(result)) == 0;
    Check(same, "source additional rotation product differs from actual NEW");
    Check(originals == before && std::fegetround() == mode, "selected turn owns no model or rounding state");
    const auto after = std::array<Math::Vector, 2>{by, rotation};
    Check(std::memcmp(inputs.data(), after.data(), sizeof(inputs)) == 0, "raw source turn inputs remain unchanged");
    if (!same) Report(result.data(), original.regs().VF[1].F, 4, trial, 0xa70);
    return same;
}
bool Euler(unsigned trial, uint32_t& seed, int mode) {
    Math::Vector angles_now, angles_next, move_now, move_next;
    Input(angles_now, seed, trial); Input(angles_next, seed, trial);
    Input(move_now, seed, trial); Input(move_next, seed, trial);
    if (trial % 5 >= 2) {
        // Authored-range angles (radians, tenths of a turn) and finite shares.
        for (auto* angles : {&angles_now, &angles_next})
            for (auto& lane : *angles) lane = static_cast<float>(static_cast<int32_t>(Next(seed))) / 268435456.0f;
        angles_next[3] = static_cast<float>(Next(seed) % 1025) / 1024.0f;
    }
    if (trial % 7 == 0) angles_now = {0, 0, 0, Float(Next(seed))};
    if (trial % 11 == 0) angles_next = {angles_now[0], angles_now[1], angles_now[2], 1};
    if (trial % 13 == 0) move_next = move_now;
    VuUnit original(0);
    original.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    RandomState(original, seed);
    std::memcpy(original.regs().VF[31].F, angles_now.data(), 16);
    std::memcpy(original.regs().VF[30].F, angles_next.data(), 16);
    std::memcpy(original.regs().VF[29].F, move_now.data(), 16);
    std::memcpy(original.regs().VF[28].F, move_next.data(), 16);
    const auto inputs = std::array<Math::Vector, 4>{angles_now, angles_next, move_now, move_next};
    VuInterpreterTier model;
    ++originals[4];
    model.Run(original, 0x818);
    Check(original.regs().TPC == 0xa70, "joint euler blend reaches reviewed return");
    const auto before = originals;
    ++selected[4];
    const auto result = Math::EulerRotation(angles_now, angles_next, move_now, move_next);
    const bool same = std::memcmp(result.rotation.data(), original.regs().VF[1].F, 16) == 0 &&
        std::memcmp(result.move.data(), original.regs().VF[2].F, 16) == 0;
    Check(same, "source euler blend rotation/move differ from actual NEW");
    Check(originals == before && std::fegetround() == mode && result.move[3] == 1,
        "selected euler blend owns no model, restores rounding and publishes affine move");
    const auto after = std::array<Math::Vector, 4>{angles_now, angles_next, move_now, move_next};
    Check(std::memcmp(inputs.data(), after.data(), sizeof(inputs)) == 0, "raw source euler inputs remain unchanged");
    if (!same) {
        Report(result.rotation.data(), original.regs().VF[1].F, 4, trial, 0x818);
        Report(result.move.data(), original.regs().VF[2].F, 4, trial, 0x818);
    }
    return same;
}
bool Hierarchy(int mode) {
    VuUnit original(0);
    original.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    Math::Matrix parent{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    const Math::Vector authored{Float(0xbcd903ab), Float(0xbac794af), Float(0x3c151e68), Float(0x3f7fe636)};
    for (unsigned joint = 0; joint < 128; ++joint) {
        const bool scaled = joint % 2;
        const Math::Vector scale{joint % 3 ? 1.01f : -1, 0.99f, 1, Float(0x7fffffff)};
        const Math::Vector parent_scale{1.01f, 0.99f, 1, 0};
        const Math::Vector move{static_cast<float>(joint) / 64, 0.1f, -0.25f, Float(0xffffffff)};
        std::memcpy(original.regs().VF[31].F, authored.data(), 16);
        std::memcpy(original.regs().VF[29].F, parent_scale.data(), 16);
        std::memcpy(original.regs().VF[30].F, scale.data(), 16);
        VuInterpreterTier model;
        ++originals[scaled];
        model.Run(original, scaled ? 0x668 : 0x548);
        ++selected[scaled];
        const auto basis = Math::RotationBasis(authored, parent_scale, scaled ? &scale : nullptr);
        const bool same_basis = std::memcmp(basis.data(), original.regs().VF[1].F, sizeof(basis)) == 0;
        Check(same_basis, "OLD authored rotation retains exact basis through warm hierarchy state");
        std::memcpy(original.regs().VF[4].F, move.data(), 16);
        std::memcpy(original.regs().VF[5].F, parent.data(), sizeof(parent));
        ++originals[2];
        model.Run(original, 0x790);
        ++selected[2];
        const auto result = Math::MultiplyByParent({basis[0], basis[1], basis[2], move}, parent);
        const bool same = std::memcmp(result.data(), original.regs().VF[1].F, sizeof(result)) == 0;
        Check(same, "parent hierarchy composition consumes the exact previous source output");
        Check(result[3][3] == 1 && std::fegetround() == mode,
            "translation NaN w is unused and caller rounding is restored through the hierarchy");
        if (!same || !same_basis) return false;
        parent = result;
    }
    return true;
}
}

int main() {
    const int previous = std::fegetround();
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "set joint matrix test rounding");
        if (!Hierarchy(mode)) return 1;
        uint32_t seed = 0x4d415452;
        for (unsigned trial = 0; trial < 4000; ++trial)
            if (!Basis(trial, seed, mode, false) || !Basis(trial, seed, mode, true) ||
                !Parent(trial, seed, mode) || !Turn(trial, seed, mode) || !Euler(trial, seed, mode)) return 1;
    }
    std::fesetround(previous);
    std::printf("MathLibraryJointMatrixOracle checks=%u failures=%u unscaled=%llu scaled=%llu parent=%llu turn=%llu euler=%llu\n",
        checks, failures, static_cast<unsigned long long>(selected[0]),
        static_cast<unsigned long long>(selected[1]), static_cast<unsigned long long>(selected[2]),
        static_cast<unsigned long long>(selected[3]), static_cast<unsigned long long>(selected[4]));
    return failures ? 1 : 0;
}
