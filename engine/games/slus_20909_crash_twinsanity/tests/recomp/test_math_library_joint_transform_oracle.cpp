#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_transform_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_dispatch_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_observations.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle_artifacts.h"
#include "games/slus_20909_crash_twinsanity/hle/math/joint_library.h"
#include "engine/vu/vu_unit.h"
#include "math_library_joint_image.h"

#include <cfenv>
#include <cstdio>
#include <cstring>
#include <stdexcept>

uint8_t* MathLibraryJointTransformOracle::Memory::Translate(uint32_t address, size_t length) {
    address &= 0x1fffffffu;
    if (address > bytes.size() || length > bytes.size() - address) return nullptr;
    return bytes.data() + address;
}
uint32_t MathLibraryJointTransformOracle::Memory::Read32(uint32_t address) {
    uint32_t result;
    std::memcpy(&result, Translate(address, 4), 4);
    return result;
}
void MathLibraryJointTransformOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, 4), &value, 4);
}

namespace {
using Oracle = MathLibraryJointTransformOracle;
using Library = title::JointLibrary;
using Vector = title::JointMath::Vector;
unsigned checks = 0, failures = 0;
uint64_t cases = 0, written = 0, done_returns = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value && failures++ < 16) std::fprintf(stderr, "%s\n", message);
}
uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
void Fill(uint8_t* bytes, size_t size, uint32_t& seed) {
    for (size_t offset = 0; offset < size; offset += 4) {
        const auto value = Next(seed);
        std::memcpy(bytes + offset, &value, 4);
    }
}
float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
Vector Quaternion(uint32_t& seed, unsigned trial) {
    constexpr std::array<uint32_t, 8> pins{0, 0x80000000, 0x007fffff, 0x7f7fffff, 0x7f800000, 0xff800000,
                                           0x7fffffff, 0xffffffff};
    switch (trial % 9) {
    case 0: return {0, 0, 0, 1};
    case 1: return {0, 0.6f, 0, 0.8f};
    case 2: return {Float(0xbcd903ab), Float(0xbac794af), Float(0x3c151e68), Float(0x3f7fe636)};
    case 3: return {Float(pins[Next(seed) % 8]), Float(pins[Next(seed) % 8]), Float(pins[Next(seed) % 8]),
                    Float(pins[Next(seed) % 8])};
    case 4: return {Float(Next(seed)), Float(Next(seed)), Float(Next(seed)), Float(Next(seed))};
    default: {
        Vector result;
        for (auto& lane : result) lane = static_cast<float>(static_cast<int32_t>(Next(seed))) / 2147483648.0f;
        return result;
    }
    }
}
Vector Scale(uint32_t& seed, unsigned trial) {
    switch ((trial / 3) % 4) {
    case 0: return {1, 1, 1, 1};
    case 1: return {Float(Next(seed)), Float(Next(seed)), Float(Next(seed)), Float(Next(seed))};
    default: {
        Vector result;
        for (auto& lane : result) lane = static_cast<float>(static_cast<int32_t>(Next(seed))) / 268435456.0f;
        if (trial % 2) result[3] = Float(Next(seed));  // Raw w word is copied, never used.
        return result;
    }
    }
}
struct Layout { uint32_t animator, animation, parent, output, stack; bool parent_present, high_parent; unsigned alias; };
Layout Place(unsigned trial, uint32_t segment) {
    Layout layout{};
    const unsigned alias = (trial / 7) % 8;
    layout.alias = alias;
    layout.animator = 0x100u + trial % 16;
    layout.animation = 0x200u + (trial / 16) % 16;
    layout.parent = 0x300u + (trial / 5) % 16;
    layout.output = 0x400u + (trial / 11) % 16;
    layout.stack = 0xf00;
    layout.parent_present = (trial / 2) % 3 != 0;
    layout.high_parent = trial % 97 == 0;  // Nonzero register with a zero low word still composes.
    if (alias == 1) layout.animation = layout.animator - 0x40;  // Scale/bits publication rewrites the flags.
    if (alias == 2) layout.animation = layout.parent;            // Parent scale/bits are rewritten first.
    if (alias == 3) layout.output = layout.parent;               // Output overwrites the consumed parent.
    if (alias == 4) layout.output = layout.animator;             // Output overwrites the consumed pose.
    if (alias == 5) layout.animation = layout.animator + 0x10;   // Bits/scale land on rotation/scale words.
    if (alias == 6) layout.output = layout.animation + 0x10;     // Output overwrites the published scale.
    for (uint32_t* address : {&layout.animator, &layout.animation, &layout.parent, &layout.output, &layout.stack})
        *address |= segment;
    return layout;
}
void Fixture(CpuContext& context, Oracle::Memory& memory, uint32_t& seed, unsigned trial, Layout& layout) {
    Fill(reinterpret_cast<uint8_t*>(&context), sizeof(context), seed);
    Fill(memory.bytes.data(), memory.bytes.size(), seed);
    const uint32_t segment = std::array<uint32_t, 3>{0, 0x80000000, 0xa0000000}[trial % 3];
    layout = Place(trial, segment);
    auto& regs = context.cpuRegs.GPR.r;
    regs[0].UD[0] = 0;
    context.vuRegs.VI[0] = 0;
    context.vuRegs.VF[0] = {};
    context.vuRegs.VF[0].F[3] = 1;
    regs[4].UL[0] = layout.animator;
    regs[5].UD[0] = layout.parent_present ? (layout.high_parent ? uint64_t{1} << 40 : layout.parent) : 0;
    regs[6].UD[0] = (trial / 64) % 2 ? (trial % 13 ? Next(seed) | 1 : uint64_t{1} << 35) : 0;
    regs[7].UL[0] = layout.output;
    regs[29].UL[0] = layout.stack;
    // All 32 active flag combinations, with the done bit on one sixth of cases.
    const uint32_t low = trial % 32 | (trial % 64 >= 32 && trial % 3 == 0 ? 0x20u : 0u);
    const uint64_t flags = (static_cast<uint64_t>(Next(seed)) << 32) | (Next(seed) & ~0x3fu) | low;
    const auto rotation = Quaternion(seed, trial), own = Scale(seed, trial), parent_scale = Scale(seed, trial + 1);
    std::memcpy(memory.Translate((layout.animator + 0x10) & ~15u, 16), rotation.data(), 16);
    std::memcpy(memory.Translate((layout.animator + 0x30) & ~15u, 16), own.data(), 16);
    if (trial % 4 == 0) {
        Vector translation{Float(Next(seed)), Float(Next(seed)), Float(Next(seed)), Float(Next(seed))};
        std::memcpy(memory.Translate((layout.animator + 0x20) & ~15u, 16), translation.data(), 16);
    }
    // Scalar fields last: a low pose alignment physically overlaps its own
    // rounded rotation quadword, and the pointer inputs must stay valid.
    std::memcpy(memory.Translate(layout.animator, 8), &flags, 8);
    memory.Write32(layout.animator + 8, layout.animation);
    if (layout.parent_present && !layout.high_parent) {
        std::memcpy(memory.Translate((layout.parent + 0x40) & ~15u, 16), parent_scale.data(), 16);
        if (trial % 5 == 0) {
            for (unsigned row = 0; row < 4; ++row) {
                const auto basis = Quaternion(seed, trial + row);
                std::memcpy(memory.Translate((layout.parent + row * 16) & ~15u, 16), basis.data(), 16);
            }
        }
    }
}
void Difference(const CpuContext& actual, const CpuContext& expected, unsigned trial) {
    for (size_t offset = 0; offset < offsetof(CpuContext, vuRegs); offset += 4) {
        uint32_t a, b;
        std::memcpy(&a, reinterpret_cast<const uint8_t*>(&actual) + offset, 4);
        std::memcpy(&b, reinterpret_cast<const uint8_t*>(&expected) + offset, 4);
        if (a != b) {
            std::fprintf(stderr, "trial=%u scalar+%zu selected=%08x control=%08x\n", trial, offset, a, b);
            break;
        }
    }
    for (unsigned reg = 0; reg < 9; ++reg)
        for (unsigned lane = 0; lane < 4; ++lane)
            if (actual.vuRegs.VF[reg].I[lane] != expected.vuRegs.VF[reg].I[lane])
                std::fprintf(stderr, "trial=%u VF%u[%u] selected=%08x control=%08x\n", trial, reg, lane,
                    actual.vuRegs.VF[reg].I[lane], expected.vuRegs.VF[reg].I[lane]);
}
Library::Body Install(Library& library, Oracle::Memory& memory, VuUnit& unit, bool selected) {
    Library::Body installed;
    Check(library.Install([&](uint32_t address) -> Library::Body {
        Check(address == Library::kRotate || address == Library::kTransform || address == Library::kAnimate,
            "capture exact source joint entries");
        if (address != Library::kTransform)
            return [](CpuContext&, uint32_t) { Check(false, "transform cases never dispatch the other entries"); };
        return [&](CpuContext& context, uint32_t entered) { Oracle::Invoke(context, memory, unit, entered); };
    }, [&](uint32_t address, Library::Body body) {
        Check(address == Library::kRotate || address == Library::kTransform || address == Library::kAnimate,
            "install exact source joint entries");
        if (address == Library::kTransform) installed = std::move(body);
        return true;
    }, selected), "joint transform adapter installs");
    return installed;
}
bool Compare(unsigned trial, uint32_t& seed, bool selected,
             MathLibraryOracleArtifacts& artifacts, MathLibraryJointObservations& observations) {
    Oracle::Memory expected_memory, actual_memory;
    CpuContext expected{}, actual{};
    Layout layout;
    Fixture(expected, expected_memory, seed, trial, layout);
    actual = expected; actual_memory.bytes = expected_memory.bytes;
    VuUnit expected_unit(0), actual_unit(0);
    for (auto* unit : {&expected_unit, &actual_unit})
        unit->WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    const auto dispatch = trial % 2 ? Library::kTransform : 0;
    const auto model_before = Oracle::Counts();
    artifacts.Begin({Library::kTransform, dispatch, 0, trial, 0, selected}, expected, expected_memory.bytes.data());
    artifacts.RunModel([&] { Oracle::Invoke(expected, expected_memory, expected_unit, dispatch); });
    artifacts.ModelOutput(expected, expected_memory.bytes.data());
    const auto model_after = Oracle::Counts();
    Library library(actual_memory);
    const auto installed = Install(library, actual_memory, actual_unit, selected);
    const auto before = Oracle::Counts();
    const int mode = std::fegetround();
    artifacts.CheckActualInput(actual, actual_memory.bytes.data());
    observations.Input(selected, dispatch, actual, actual_memory.bytes.data());
    const uint64_t flags = actual_memory.Read32(layout.animator);  // Pristine entry flags.
    installed(actual, dispatch);
    const auto after = Oracle::Counts();
    if (artifacts.Enabled()) artifacts.ActualOutput(actual, actual_memory.bytes.data(), library.Census());
    observations.Output(selected, actual, actual_memory.bytes.data());
    const bool same = actual_memory.bytes == expected_memory.bytes &&
        (selected ? observations.Equal(actual, expected) : std::memcmp(&actual, &expected, sizeof(actual)) == 0);
    Check(same, "complete scalar ABI, memory and source joint matrix match actual original");
    if (!same) Difference(actual, expected, trial);
    const bool done = (flags & 0x20) != 0;
    const auto counts = library.Counts();
    const auto originals = selected ? 0u : 1u;
    Check(after.generated_bodies - before.generated_bodies == originals &&
        counts.generated_body_transform == originals && counts.transforms == (selected ? 1u : 0u) &&
        counts.matrices_written == (selected && !done ? 1u : 0u) && counts.done_returns == (selected && done ? 1u : 0u),
        "actual original body executions and positive owned work match selection and done state");
    const auto bases = [](const Oracle::Counters& c) { return c.actual_bases + c.actual_scaled_bases; };
    const bool rotation = !done && (flags & 1) != 0;
    const uint64_t model_bases = bases(model_after) - bases(model_before);
    const uint64_t model_parents = model_after.actual_parent_products - model_before.actual_parent_products;
    Check(model_bases == (rotation ? 1u : 0u) && model_parents <= 1 && (model_parents == 0 || !done),
        "direct original performs exactly the source basis program call and at most one parent product");
    if (layout.alias != 1 && layout.alias != 4) {
        // Neither the scale/bits publication nor the output rewrites the flags,
        // so the reloaded flags equal the final flags apart from the done bit.
        const uint64_t final_flags = expected_memory.Read32(layout.animator);
        const bool composed = !done && layout.parent_present && (final_flags & 0x10) == 0;
        Check(model_parents == (composed ? 1u : 0u), "direct original composes exactly when the source says");
    }
    Check(bases(after) - bases(before) == (selected ? 0u : model_bases) &&
        after.actual_parent_products - before.actual_parent_products == (selected ? 0u : model_parents),
        "selected transform performs no original program call; control performs the source calls");
    Check(counts.generated_interiors == 0 && counts.unsupported_inputs == 0 && counts.rotations == 0 &&
        std::fegetround() == mode, "valid source entry requires no unsupported inputs and restores rounding");
    ++cases;
    if (selected) (done ? done_returns : written) += 1;
    return same;
}
void Interiors() {
    constexpr std::array<uint32_t, 25> interiors{
        0x294318, 0x294324, 0x29432c, 0x294334, 0x294340, 0x294344, 0x294358, 0x294368,
        0x294374, 0x29438c, 0x294398, 0x2943a4, 0x2943ac, 0x2943e0, 0x2943e8, 0x2943f8,
        0x29440c, 0x294418, 0x29441c, 0x294424, 0x29442c, 0x294454, 0x29445c, 0x294464,
        0x294478};
    for (bool selected : {false, true}) {
        Oracle::Memory memory;
        VuUnit unit(0);
        unit.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
        MathLibraryDispatchFixture::Reset();
        MathLibraryDispatchFixture::Bind(Library::kTransform,
            [&](CpuContext& context, uint32_t entry) { Oracle::Invoke(context, memory, unit, entry); });
        for (uint32_t other : {Library::kRotate, Library::kAnimate})
            MathLibraryDispatchFixture::Bind(other,
                [](CpuContext&, uint32_t) { Check(false, "transform interiors never dispatch the other entries"); });
        Library library(memory);
        Check(library.Install(DispatchTable::Capture, DispatchTable::Install, selected),
            "joint actual dispatch install");
        uint32_t seed = 0x4a4f494e;
        for (uint32_t entry : interiors) {
            CpuContext actual{};
            Layout layout;
            Fixture(actual, memory, seed, static_cast<unsigned>(entry % 61), layout);
            auto& regs = actual.cpuRegs.GPR.r;
            regs[8].UD[0] = layout.animator;
            regs[9].UD[0] = layout.parent_present ? layout.parent : 0;
            regs[4].SD[0] = static_cast<int32_t>(layout.animation);
            regs[29].SD[0] = static_cast<int32_t>(layout.stack - 16);
            std::memcpy(&regs[10].UD[0], memory.Translate(layout.animator, 8), 8);
            regs[3].UD[0] = regs[10].UD[0];
            regs[5].SD[0] = 1;
            CpuContext expected = actual;
            auto expected_memory = memory;
            VuUnit expected_unit(0);
            expected_unit.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
            Oracle::Invoke(expected, expected_memory, expected_unit, entry);
            Check(DispatchTable::CanResumeAt(Library::kTransform, entry) &&
                !DispatchTable::CanResumeAt(Library::kTransform, entry + 1),
                "only reviewed exact original interiors keep replay proof");
            Check(DispatchTable::InvokeAt(Library::kTransform, entry, actual) &&
                std::memcmp(&actual, &expected, sizeof(actual)) == 0 && memory.bytes == expected_memory.bytes,
                "joint transform interior resumes actual frame without full-entry replay");
        }
        Check(!selected || !DispatchTable::CanResumeAt(Library::kTransform, 0),
            "selected full source entry gains no generated replay proof");
        const auto counts = library.Counts();
        Check(counts.generated_body_transform == interiors.size() &&
            counts.generated_interiors == (selected ? interiors.size() : 0u) && counts.transforms == 0,
            "interior delegation counts every actual original body");
    }
    MathLibraryDispatchFixture::Reset();
}
// A declined entry never throws: it is counted per entry and reason, then the
// captured original runs once on the unchanged entry context.
void Refuse(Library& library, Library::Body& retained, CpuContext& context, Oracle::Memory& memory,
            unsigned& originals, Library::Refusal reason, const char* message) {
    const auto before = context;
    const auto bytes = memory.bytes;
    const auto counts = library.Counts();
    bool threw = false;
    try { retained(context, 0); }
    catch (...) { threw = true; }
    const auto after = library.Counts();
    Check(!threw && std::memcmp(&context, &before, sizeof(context)) == 0 && bytes == memory.bytes &&
        originals == after.unsupported_inputs && after.refused_transform == counts.refused_transform + 1 &&
        after.unsupported_inputs == counts.unsupported_inputs + 1 && after.transforms == counts.transforms &&
        library.LastRefusal() == reason, message);
}
// The title's stack lives in the mirrored scratchpad (every anchor's boundary
// `$sp` is 0x70004220, above the 16 KB end), and JointAnimator objects are
// stack locals: one may straddle the 0x70004000 mirror seam, where a single
// host translation of the whole object fails while the generated quad loads
// read it through the mirror (engine/ee/mem/scratchpad.h: offset = address & 0x3fff).
class MirroredMemory final : public ps2r::ipu::GuestMemory {
public:
    std::array<uint8_t, 0x1000> ram{};
    std::array<uint8_t, 0x4000> scratchpad{};
    uint8_t* Translate(uint32_t address, size_t length) override {
        if ((address & 0x70000000u) == 0x70000000u) {
            const uint32_t offset = address & 0x3fffu;
            return length <= scratchpad.size() - offset ? scratchpad.data() + offset : nullptr;
        }
        address &= 0x1fffffffu;
        return address < ram.size() && length <= ram.size() - address ? ram.data() + address : nullptr;
    }
    uint32_t Read32(uint32_t address) override {
        uint32_t value = 0;
        if (auto* data = Translate(address, 4)) std::memcpy(&value, data, 4);
        return value;
    }
    void Write32(uint32_t address, uint32_t value) override {
        if (auto* data = Translate(address, 4)) std::memcpy(data, &value, 4);
    }
};
void MirrorSeamPins() {
    std::array<CpuContext, 2> results{};
    std::array<std::array<uint8_t, 0x1000>, 2> rams{};
    unsigned originals = 0;
    for (unsigned straddling = 0; straddling < 2; ++straddling) {
        MirroredMemory memory;
        Library library(memory);
        Library::Body retained;
        Check(library.Install([&](uint32_t) -> Library::Body {
            return [&](CpuContext&, uint32_t) { ++originals; };
        }, [&](uint32_t address, Library::Body body) {
            if (address == Library::kTransform) retained = std::move(body);
            return true;
        }, true), "transform mirror seam fixture installs");
        const uint32_t animator = straddling ? 0x70003fd0u : 0x70001000u;
        CpuContext context{};
        context.vuRegs.VF[0].F[3] = 1;
        context.cpuRegs.GPR.r[4].UL[0] = animator;
        context.cpuRegs.GPR.r[7].UL[0] = 0x400;
        context.cpuRegs.GPR.r[29].UL[0] = 0x70004020u;
        memory.Write32(animator, 0x7);  // rotation, translation and scale present
        memory.Write32(animator + 8, 0x200);
        const uint32_t rotation[]{0, 0, 0, 0x3f800000u}, translation[]{0x3f800000u, 0x40000000u, 0x40400000u, 0x3f800000u},
            scale[]{0x40000000u, 0x40000000u, 0x40000000u, 0x3f800000u};
        for (unsigned lane = 0; lane < 4; ++lane) {
            memory.Write32(animator + 0x10 + lane * 4, rotation[lane]);
            memory.Write32(animator + 0x20 + lane * 4, translation[lane]);
            memory.Write32(animator + 0x30 + lane * 4, scale[lane]);
        }
        retained(context, 0);
        const auto counts = library.Counts();
        Check(counts.matrices_written == 1 && counts.refused_transform == 0 && counts.unsupported_inputs == 0 &&
            counts.generated_body_transform == 0 && originals == 0,
            straddling ? "transform whose animator straddles the scratchpad mirror seam is owned, not refused"
                       : "transform of a scratchpad animator is owned");
        context.cpuRegs.GPR.r[4].UD[0] = context.cpuRegs.GPR.r[8].UD[0] = 0;  // animator address
        results[straddling] = context;
        rams[straddling] = memory.ram;
    }
    Check(std::memcmp(&results[0], &results[1], sizeof(CpuContext)) == 0 && rams[0] == rams[1],
        "seam-straddling transform publishes the same registers, output matrix and animation fields");
}
void FailurePins() {
    Oracle::Memory memory;
    for (bool throwing : {false, true}) {
        std::array<Library::Body, 2> retained;
        unsigned attempts = 0, originals = 0;
        {
            Library library(memory);
            bool refused = false;
            try {
                refused = !library.Install([&](uint32_t) -> Library::Body {
                    return [&](CpuContext&, uint32_t) { ++originals; };
                }, [&](uint32_t address, Library::Body body) {
                    // The second (transform) publication fails; its rollback of the first is rejected.
                    if (attempts++ >= 2) throw std::runtime_error("rollback rejected");
                    retained[address == Library::kTransform] = std::move(body);
                    if (address != Library::kTransform) return true;
                    CpuContext context{};
                    retained[1](context, 0);
                    if (throwing) throw std::runtime_error("publication failed");
                    return false;
                }, true);
            } catch (const std::runtime_error& error) {
                refused = std::string(error.what()) == "publication failed";
            }
            Check(refused && originals == 1 && attempts == 4 && library.Counts().transforms == 0,
                "failed second publication is inactive and restores the first even when rollback throws");
        }
        for (auto& body : retained) { CpuContext context{}; body(context, 0); }
        Check(originals == 3, "both failed joint callbacks survive quiescent owner destruction");
    }
    Library::Body retained;
    unsigned originals = 0;
    {
        Library library(memory);
        Check(library.Install([&](uint32_t) -> Library::Body {
            return [&](CpuContext&, uint32_t) { ++originals; };
        }, [&](uint32_t address, Library::Body body) {
            if (address == Library::kTransform) retained = std::move(body);
            return true;
        }, true), "joint transform lifetime fixture installs");
        const auto valid = [&](CpuContext& context) {
            context = {};
            context.vuRegs.VF[0].F[3] = 1;
            context.cpuRegs.GPR.r[4].UL[0] = 0x100;
            context.cpuRegs.GPR.r[5].UL[0] = 0x300;
            context.cpuRegs.GPR.r[7].UL[0] = 0x400;
            context.cpuRegs.GPR.r[29].UL[0] = 0xf00;
            std::memset(memory.Translate(0x100, 0x40), 0, 0x40);
            memory.Write32(0x100, 0x1f);
            memory.Write32(0x108, 0x200);
        };
        CpuContext context{};
        unsigned refusals = 0;
        using R = Library::Refusal;
        const auto refuse = [&](R reason, const char* message) {
            Refuse(library, retained, context, memory, originals, reason, message);
            ++refusals;
        };
        valid(context); context.vuRegs.VI[0] = 3; refuse(R::State, "noncanonical VI0 declines unchanged");
        valid(context); context.vuRegs.VF[0].F[1] = 1; refuse(R::State, "noncanonical VF0 declines unchanged");
        valid(context); context.cpuRegs.GPR.r[0].UD[0] = 1; refuse(R::State, "noncanonical zero register declines unchanged");
        valid(context); context.cpuRegs.GPR.r[4].UL[0] = 0xffc; refuse(R::Objects, "unmapped flags decline unchanged");
        valid(context); context.cpuRegs.GPR.r[4].UL[0] = 0xfffffff8; refuse(R::Objects, "wrapping animator declines unchanged");
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0xf08; refuse(R::Frame, "unaligned frame declines unchanged");
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0; refuse(R::Frame, "frame below zero declines unchanged");
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0x130; refuse(R::Frame, "frame aliasing the pose declines unchanged");
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0x210; refuse(R::Frame, "frame aliasing the animation declines");
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0x80000310; refuse(R::Frame, "segment-aliased frame on parent declines");
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0x410; refuse(R::Frame, "frame aliasing the output declines unchanged");
        valid(context); memory.Write32(0x108, 0xffb0); refuse(R::Objects, "unmapped animation declines unchanged");
        valid(context); memory.Write32(0x108, 0xffffffff); refuse(R::Objects, "wrapping animation declines unchanged");
        valid(context); context.cpuRegs.GPR.r[7].UL[0] = 0xfd0; refuse(R::Objects, "unmapped output declines unchanged");
        valid(context); context.cpuRegs.GPR.r[5].UL[0] = 0xfc0; refuse(R::Objects, "unmapped parent declines unchanged");
        const auto counts = library.Counts();
        Check(counts.unsupported_inputs == refusals && refusals == 15 && counts.refused_transform == 15 &&
            counts.refusal_state == 3 && counts.refusal_objects == 6 && counts.refusal_frame == 6 &&
            counts.refusal_channels == 0 && counts.faults == 0 && counts.generated_body_transform == 15 &&
            originals == 15,
            "invalid source frames/state have an actual per-reason refusal count and generated fallbacks");
        originals = 0;
        // A done animator is honored with otherwise unusable pointers and frame.
        valid(context);
        memory.Write32(0x100, 0x20);
        context.cpuRegs.GPR.r[29].UL[0] = 1;
        context.cpuRegs.GPR.r[7].UL[0] = 0xffffffff;
        context.cpuRegs.GPR.r[5].UD[0] = 0xffffffffffffffffull;
        const auto before = context;
        const auto bytes = memory.bytes;
        retained(context, 0);
        auto expected = before;
        auto& regs = expected.cpuRegs.GPR.r;
        regs[8].UD[0] = 0x100; regs[9].UD[0] = 0xffffffffffffffffull;
        regs[3].UD[0] = 0x20; regs[2].UD[0] = 0x20; regs[29].SD[0] = 1;
        Check(std::memcmp(&context, &expected, sizeof(context)) == 0 && bytes == memory.bytes && originals == 0 &&
            library.Counts().done_returns == 1 && library.Counts().matrices_written == 0,
            "done animator returns through the entry scalar ABI without reads, writes or refusal");
    }
    MirrorSeamPins();
    CpuContext context{};
    retained(context, 0);
    Check(originals == 1, "successful joint transform callback safely forwards after quiescent owner teardown");
}
}

int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryJointTransformOracle", Oracle::SourceDigest(),
        Oracle::Memory{}.bytes.size(), 0);
    artifacts.Fixture("standard_program", math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    MathLibraryJointObservations observations(argc == 3 ? argv[2] : nullptr, Oracle::Memory{}.bytes.size(),
        MathLibraryJointObservations::Scope::Transform);
    const int previous = std::fegetround();
    Interiors();
    FailurePins();
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "set joint transform rounding");
        uint32_t seed = 0x5452464d;
        for (unsigned trial = 0; trial < 2000; ++trial) {
            auto control_seed = seed;
            if (!Compare(trial, seed, true, artifacts, observations) ||
                !Compare(trial, control_seed, false, artifacts, observations)) return 1;
            Check(seed == control_seed, "actual control and selected consume the same fixture stream");
        }
    }
    std::fesetround(previous);
    if (!failures) { observations.Finish(); artifacts.Finish(); }
    const auto counts = Oracle::Counts();
    std::printf("MathLibraryJointTransformOracle checks=%u failures=%u cases=%llu written=%llu done=%llu "
                "actual_bodies=%llu actual_bases=%llu actual_scaled_bases=%llu actual_parent_products=%llu\n",
        checks, failures, static_cast<unsigned long long>(cases), static_cast<unsigned long long>(written),
        static_cast<unsigned long long>(done_returns), static_cast<unsigned long long>(counts.generated_bodies),
        static_cast<unsigned long long>(counts.actual_bases), static_cast<unsigned long long>(counts.actual_scaled_bases),
        static_cast<unsigned long long>(counts.actual_parent_products));
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryJointTransformOracle: %s\n", error.what());
        return 1;
    }
}
