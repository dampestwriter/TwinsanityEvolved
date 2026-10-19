#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_adapter_oracle.h"
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

uint8_t* MathLibraryJointAdapterOracle::Memory::Translate(uint32_t address, size_t length) {
    address &= 0x1fffffffu;
    if (address > bytes.size() || length > bytes.size() - address) return nullptr;
    return bytes.data() + address;
}
uint32_t MathLibraryJointAdapterOracle::Memory::Read32(uint32_t address) {
    uint32_t result;
    std::memcpy(&result, Translate(address, 4), 4);
    return result;
}
void MathLibraryJointAdapterOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, 4), &value, 4);
}

namespace {
using Oracle = MathLibraryJointAdapterOracle;
using Library = title::JointLibrary;
unsigned checks = 0, failures = 0;
uint64_t cases = 0;
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
void Fixture(CpuContext& context, Oracle::Memory& memory, uint32_t& seed, unsigned trial) {
    Fill(reinterpret_cast<uint8_t*>(&context), sizeof(context), seed);
    Fill(memory.bytes.data(), memory.bytes.size(), seed);
    const uint32_t segment = std::array<uint32_t, 3>{0, 0x80000000, 0xa0000000}[trial % 3];
    const uint32_t pose = segment | (0x100u + trial % 16);
    const uint32_t joint = trial % 5 ? segment | (0x280u + trial % 16) : pose - 16;
    auto& regs = context.cpuRegs.GPR.r;
    const uint64_t zero = std::array<uint64_t, 4>{0, 1, 17, UINT64_MAX}[trial % 4];
    regs[0].UD[0] = zero;
    regs[4].UD[0] = static_cast<uint64_t>(pose) - zero;
    regs[29].UL[0] = segment | 0xf00;
    const uint32_t fields = (memory.Read32(pose) & ~1u) | (trial / 4 % 2);
    // VI0 is a source-entry invariant; malformed values are refusal cases
    // below. VF0 stays arbitrary because the original publishes its constant.
    context.vuRegs.VI[0] = 0;
    if (trial % 3) {
        context.fpuRegs.fpr[12].f = static_cast<float>(static_cast<int>(trial % 25) - 4) / 16;
        const title::JointMath::Vector current{0, 0.6f, 0, 0.8f}, target{0, 0, 0, 1};
        std::memcpy(memory.Translate((pose + 16) & ~15u, 16), current.data(), 16);
        if (trial % 5) std::memcpy(memory.Translate((joint + 16) & ~15u, 16), current.data(), 16);
        std::memcpy(context.vuRegs.VF[1].F, target.data(), 16);
    }
    // Keep pointer inputs valid even when an intentionally unaligned source
    // object aliases its own rounded quaternion quadword.
    memory.Write32(pose, fields);
    memory.Write32(pose + 4, joint);
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
    for (unsigned lane = 0; lane < 4; ++lane)
        std::fprintf(stderr, "trial=%u quaternion[%u] selected=%08x control=%08x\n", trial, lane,
            actual.vuRegs.VF[1].I[lane], expected.vuRegs.VF[1].I[lane]);
}
bool Compare(unsigned trial, uint32_t& seed, bool selected,
             MathLibraryOracleArtifacts& artifacts, MathLibraryJointObservations& observations) {
    Oracle::Memory expected_memory, actual_memory;
    CpuContext expected{}, actual{};
    Fixture(expected, expected_memory, seed, trial);
    actual = expected; actual_memory.bytes = expected_memory.bytes;
    VuUnit expected_unit(0), actual_unit(0);
    for (auto* unit : {&expected_unit, &actual_unit})
        unit->WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    const auto dispatch = trial % 2 ? Library::kRotate : 0;
    artifacts.Begin({Library::kRotate, dispatch, 0, trial, 0, selected}, expected, expected_memory.bytes.data());
    artifacts.RunModel([&] { Oracle::Invoke(expected, expected_memory, expected_unit, dispatch); });
    artifacts.ModelOutput(expected, expected_memory.bytes.data());
    Library library(actual_memory);
    Library::Body installed;
    Check(library.Install([&](uint32_t address) -> Library::Body {
        Check(address == Library::kRotate || address == Library::kTransform || address == Library::kAnimate,
            "capture exact source joint entries");
        if (address != Library::kRotate)
            return [](CpuContext&, uint32_t) { Check(false, "RotateJoint cases never dispatch the other entries"); };
        return [&](CpuContext& context, uint32_t entered) {
            Oracle::Invoke(context, actual_memory, actual_unit, entered);
        };
    }, [&](uint32_t address, Library::Body body) {
        Check(address == Library::kRotate || address == Library::kTransform || address == Library::kAnimate,
            "install exact source joint entries");
        if (address == Library::kRotate) installed = std::move(body);
        return true;
    }, selected), "joint adapter installs");
    const auto before = Oracle::Counts();
    const int mode = std::fegetround();
    artifacts.CheckActualInput(actual, actual_memory.bytes.data());
    observations.Input(selected, dispatch, actual, actual_memory.bytes.data());
    installed(actual, dispatch);
    const auto after = Oracle::Counts();
    if (artifacts.Enabled()) artifacts.ActualOutput(actual, actual_memory.bytes.data(), library.Census());
    observations.Output(selected, actual, actual_memory.bytes.data());
    const bool same = actual_memory.bytes == expected_memory.bytes &&
        (selected ? observations.Equal(actual, expected) :
                    std::memcmp(&actual, &expected, sizeof(actual)) == 0);
    Check(same, "complete scalar ABI, memory and source joint result match actual original");
    if (!same) Difference(actual, expected, trial);
    const auto counts = library.Counts();
    const auto originals = selected ? 0u : 1u;
    Check(after.generated_bodies - before.generated_bodies == originals &&
        after.actual_interpolations - before.actual_interpolations == originals &&
        counts.generated_body_rotate == originals && counts.rotations == (selected ? 1u : 0u),
        "actual original body/model executions and positive owned work match selection");
    Check(counts.generated_interiors == 0 && counts.unsupported_inputs == 0 &&
        std::fegetround() == mode, "valid source entry requires no unsupported inputs and restores rounding");
    ++cases;
    return same;
}
void Interiors() {
    for (bool selected : {false, true}) {
        Oracle::Memory memory;
        VuUnit unit(0);
        unit.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
        MathLibraryDispatchFixture::Reset();
        MathLibraryDispatchFixture::Bind(Library::kRotate,
            [&](CpuContext& context, uint32_t entry) { Oracle::Invoke(context, memory, unit, entry); });
        for (uint32_t other : {Library::kTransform, Library::kAnimate})
            MathLibraryDispatchFixture::Bind(other,
                [](CpuContext&, uint32_t) { Check(false, "RotateJoint interiors never dispatch the other entries"); });
        Library library(memory);
        Check(library.Install(DispatchTable::Capture, DispatchTable::Install, selected),
            "joint actual dispatch install");
        for (uint32_t entry : {0x297b6cu, 0x297b78u, 0x297b90u}) {
            CpuContext actual{}, expected{};
            uint32_t seed = 0x12345678;
            Fixture(actual, memory, seed, 1);
            const uint32_t pose = static_cast<uint32_t>(actual.cpuRegs.GPR.r[4].UD[0] + actual.cpuRegs.GPR.r[0].UD[0]);
            actual.cpuRegs.GPR.r[5].UD[0] = pose;
            std::memcpy(&actual.cpuRegs.GPR.r[4].UD[0], memory.Translate(pose, 8), 8);
            actual.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(actual.cpuRegs.GPR.r[29].UL[0] - 32);
            const title::JointMath::Vector shares{0.25f, 0.25f, 0.25f, 1}, from{0, 0, 0, 1};
            const auto frame = actual.cpuRegs.GPR.r[29].UL[0];
            std::memcpy(memory.Translate(frame, 16), shares.data(), 16);
            std::memcpy(memory.Translate(frame + 16, 16), from.data(), 16);
            expected = actual;
            auto expected_memory = memory;
            VuUnit expected_unit(0);
            expected_unit.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
            Oracle::Invoke(expected, expected_memory, expected_unit, entry);
            Check(DispatchTable::CanResumeAt(Library::kRotate, entry) &&
                !DispatchTable::CanResumeAt(Library::kRotate, entry + 1),
                "only reviewed exact original interiors keep replay proof");
            Check(DispatchTable::InvokeAt(Library::kRotate, entry, actual) &&
                std::memcmp(&actual, &expected, sizeof(actual)) == 0 && memory.bytes == expected_memory.bytes,
                "joint interior resumes actual frame without full-entry replay");
        }
        Check(!selected || !DispatchTable::CanResumeAt(Library::kRotate, 0),
            "selected full source entry gains no generated replay proof");
        const auto counts = library.Counts();
        Check(counts.generated_body_rotate == 3 && counts.generated_interiors == (selected ? 3u : 0u) &&
            counts.rotations == 0, "interior delegation counts every actual original body");
    }
    MathLibraryDispatchFixture::Reset();
}
// The title's stack lives in the mirrored scratchpad (every anchor's boundary
// `$sp` is 0x70004220, above the 16 KB end): a stack object may straddle the
// 0x70004000 mirror seam, where one host translation of the whole object fails
// while the generated loads read it quad by quad. The address-map rule is
// engine/ee/mem/scratchpad.h (offset = address & 0x3fff).
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
    // The same rotation with its 32-byte callee frame inside the scratchpad
    // and straddling the seam: identical results, the frame quads landing at
    // the mirrored offsets, no refusal and no original execution.
    std::array<CpuContext, 2> results{};
    std::array<std::array<uint8_t, 0x4000>, 2> scratchpads{};
    unsigned originals = 0;
    for (unsigned straddling = 0; straddling < 2; ++straddling) {
        MirroredMemory memory;
        Library library(memory);
        Library::Body retained;
        Check(library.Install([&](uint32_t) -> Library::Body {
            return [&](CpuContext&, uint32_t) { ++originals; };
        }, [&](uint32_t address, Library::Body body) {
            if (address == Library::kRotate) retained = std::move(body);
            return true;
        }, true), "mirror seam fixture installs");
        CpuContext context{};
        context.vuRegs.VF[0].F[3] = 1;
        context.cpuRegs.GPR.r[29].UL[0] = straddling ? 0x70004010u : 0x70001000u;
        context.cpuRegs.GPR.r[4].UL[0] = 0x100;
        memory.Write32(0x100, 1);  // current pose: from = (pose + 16) & ~15
        for (unsigned lane = 0; lane < 4; ++lane) {
            memory.Write32(0x110 + lane * 4, 0x3f000000u + lane * 0x100000u);
            context.vuRegs.VF[1].I[lane] = 0x3e800000u + lane * 0x100000u;
        }
        context.fpuRegs.fpr[12].f = 0.25f;
        retained(context, 0);
        const auto counts = library.Counts();
        Check(counts.rotations == 1 && counts.refused_rotate == 0 && counts.unsupported_inputs == 0 &&
            counts.generated_body_rotate == 0 && originals == 0,
            straddling ? "rotation whose frame straddles the scratchpad mirror seam is owned, not refused"
                       : "rotation inside the scratchpad is owned");
        results[straddling] = context;
        scratchpads[straddling] = memory.scratchpad;
    }
    results[1].cpuRegs.GPR.r[29] = results[0].cpuRegs.GPR.r[29];
    Check(std::memcmp(&results[0], &results[1], sizeof(CpuContext)) == 0,
        "seam-straddling rotation publishes the same registers as the contiguous one");
    // Frame quads: contiguous at 0x0fe0/0x0ff0; straddling at 0x3ff0 and 0x0000.
    Check(std::memcmp(&scratchpads[0][0x0fe0], &scratchpads[1][0x3ff0], 16) == 0 &&
        std::memcmp(&scratchpads[0][0x0ff0], &scratchpads[1][0x0000], 16) == 0,
        "seam-straddling frame quads land at the mirrored scratchpad offsets");
}
void FailurePins() {
    Oracle::Memory memory;
    for (bool throwing : {false, true}) {
        Library::Body retained;
        unsigned attempts = 0, originals = 0;
        {
            Library library(memory);
            bool refused = false;
            try {
                refused = !library.Install([&](uint32_t) -> Library::Body {
                    return [&](CpuContext&, uint32_t) { ++originals; };
                }, [&](uint32_t, Library::Body body) {
                    if (attempts++ != 0) throw std::runtime_error("rollback rejected");
                    retained = std::move(body);
                    CpuContext context{};
                    retained(context, 0);
                    if (throwing) throw std::runtime_error("publication failed");
                    return false;
                }, true);
            } catch (const std::runtime_error& error) {
                refused = std::string(error.what()) == "publication failed";
            }
            Check(refused && originals == 1 && library.Counts().rotations == 0,
                "failed publication is inactive even when rollback throws");
        }
        CpuContext context{};
        retained(context, 0);
        Check(originals == 2, "failed joint callback survives quiescent owner destruction");
    }
    Library::Body retained;
    unsigned originals = 0;
    {
        Library library(memory);
        Check(library.Install([&](uint32_t) -> Library::Body {
            return [&](CpuContext&, uint32_t) { ++originals; };
        }, [&](uint32_t address, Library::Body body) {
            if (address == Library::kRotate) retained = std::move(body);
            return true;
        }, true), "joint lifetime fixture installs");
        // A declined entry never throws: the refusal is counted per entry and
        // reason, then the captured original runs once on the unchanged entry
        // context (the stub original leaves context and memory as they were).
        unsigned refusals = 0;
        const auto refuse = [&](CpuContext& context, Library::Refusal reason, const char* message) {
            const auto before = context;
            const auto bytes = memory.bytes;
            const auto counts = library.Counts();
            bool threw = false;
            try { retained(context, 0); }
            catch (...) { threw = true; }
            const auto after = library.Counts();
            ++refusals;
            Check(!threw && std::memcmp(&context, &before, sizeof(context)) == 0 && bytes == memory.bytes &&
                originals == refusals && after.refused_rotate == counts.refused_rotate + 1 &&
                after.unsupported_inputs == counts.unsupported_inputs + 1 && after.rotations == 0 &&
                library.LastRefusal() == reason, message);
        };
        for (uint32_t stack : {0u, 0xf01u, 0x110u}) {
            CpuContext context{};
            context.cpuRegs.GPR.r[29].UL[0] = stack;
            context.cpuRegs.GPR.r[4].UL[0] = 0x100;
            refuse(context, Library::Refusal::Frame,
                "invalid or overlapping source frame declines before mutation and the original runs");
        }
        CpuContext invalid{};
        invalid.cpuRegs.GPR.r[29].UL[0] = 0xf00;
        invalid.cpuRegs.GPR.r[4].UL[0] = 0x100;
        invalid.vuRegs.VI[0] = 9;
        refuse(invalid, Library::Refusal::State,
            "noncanonical entry zero-register state declines unchanged and the original runs");
        for (unsigned bad = 0; bad < 4; ++bad) {
            CpuContext context{};
            context.cpuRegs.GPR.r[29].UL[0] = 0xf00;
            context.cpuRegs.GPR.r[4].UL[0] = bad == 0 ? 4093u : bad == 2 ? 0x80000ef0u : 0x100u;
            memory.Write32(0x100, 0);
            memory.Write32(0x104, bad == 1 ? UINT32_MAX : bad == 3 ? 0xa0000ee0u : 0x280u);
            refuse(context, bad < 2 ? Library::Refusal::Objects : Library::Refusal::Frame,
                "unmapped/wrapping object or cross-segment frame alias declines unchanged and the original runs");
        }
        const auto counts = library.Counts();
        Check(counts.unsupported_inputs == 8 && counts.refused_rotate == 8 && counts.refusal_frame == 5 &&
            counts.refusal_state == 1 && counts.refusal_objects == 2 && counts.refusal_channels == 0 &&
            counts.faults == 0 && counts.generated_body_rotate == 8,
            "invalid source frames/state have an actual per-reason refusal count and generated fallbacks");
        originals = 0;
    }
    CpuContext context{};
    retained(context, 0);
    Check(originals == 1, "successful joint callback safely forwards after quiescent owner teardown");
    MirrorSeamPins();
}

}

int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryJointAdapterOracle", Oracle::SourceDigest(),
        Oracle::Memory{}.bytes.size(), 0);
    artifacts.Fixture("standard_program", math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    MathLibraryJointObservations observations(argc == 3 ? argv[2] : nullptr, Oracle::Memory{}.bytes.size());
    const int previous = std::fegetround();
    Interiors();
    FailurePins();
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "set joint adapter rounding");
        uint32_t seed = 0x524f5441;
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
    std::printf("MathLibraryJointAdapterOracle checks=%u failures=%u cases=%llu actual_bodies=%llu actual_interpolations=%llu\n",
        checks, failures, static_cast<unsigned long long>(cases),
        static_cast<unsigned long long>(counts.generated_bodies),
        static_cast<unsigned long long>(counts.actual_interpolations));
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryJointAdapterOracle: %s\n", error.what());
        return 1;
    }
}
