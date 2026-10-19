#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_animate_oracle.h"
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

uint8_t* MathLibraryJointAnimateOracle::Memory::Translate(uint32_t address, size_t length) {
    address &= 0x1fffffffu;
    if (address < kLow && length <= kLow - address) return bytes.data() + address;
    if (address >= kWindow && address - kWindow < kWindowSize && length <= kWindowSize - (address - kWindow))
        return bytes.data() + kLow + (address - kWindow);
    return nullptr;
}
uint32_t MathLibraryJointAnimateOracle::Memory::Read32(uint32_t address) {
    uint32_t result;
    std::memcpy(&result, Translate(address, 4), 4);
    return result;
}
void MathLibraryJointAnimateOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, 4), &value, 4);
}

namespace {
using Oracle = MathLibraryJointAnimateOracle;
using Library = title::JointLibrary;
using Vector = title::JointMath::Vector;
unsigned checks = 0, failures = 0;
uint64_t cases = 0, published = 0, blended = 0;
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
float Unit(uint32_t& seed) { return static_cast<float>(Next(seed) >> 8) / 16777216.0f; }
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
float Weight(uint32_t& seed, unsigned trial) {
    switch (trial % 7) {
    case 0: return 1.0f;
    case 1: return 0.5f;
    case 2: return 0.0f;
    case 3: return Float(Next(seed));                 // Any word, including NaN/Inf/negative.
    case 4: return 1.0f + Unit(seed);                  // Above one publishes raw frames.
    case 5: return Float(0x3f7fffff);                  // Largest weight below one.
    default: return Unit(seed);
    }
}
struct Layout { uint32_t animator, joint, data, stack, segment; unsigned index; };
Layout Place(unsigned trial) {
    Layout layout{};
    layout.segment = std::array<uint32_t, 3>{0, 0x80000000, 0xa0000000}[trial % 3];
    layout.animator = 0x100u + trial % 16;
    layout.joint = 0x200u + (trial / 16) % 16;
    layout.data = 0x300u + (trial / 11) % 4 * 4;
    layout.stack = 0xf00;
    layout.index = (trial / 5) % 4;
    return layout;
}
void Fixture(CpuContext& context, Oracle::Memory& memory, uint32_t& seed, unsigned trial, Layout& layout) {
    Fill(reinterpret_cast<uint8_t*>(&context), sizeof(context), seed);
    Fill(memory.bytes.data(), memory.bytes.size(), seed);
    layout = Place(trial);
    auto& regs = context.cpuRegs.GPR.r;
    regs[0].UD[0] = 0;
    context.vuRegs.VI[0] = 0;
    context.vuRegs.VF[0] = {};
    context.vuRegs.VF[0].F[3] = 1;
    regs[4].UL[0] = layout.animator | layout.segment;
    regs[5].UL[0] = layout.data | layout.segment;
    regs[29].UL[0] = layout.stack | layout.segment;
    context.fpuRegs.fpr[12].f = Weight(seed, trial);
    context.fpuRegs.fpr[13].f = trial % 13 == 0 ? Float(Next(seed)) : Unit(seed);
    // Animator: flags (all rotation/translation/scale combinations plus random
    // high bits), joint pointer, pose rotation; joint: index, pose rotation,
    // translation and additional rotation; default scale constants.
    const uint64_t flags = (static_cast<uint64_t>(Next(seed)) << 32) | (Next(seed) & ~0x7u) | (trial % 8);
    const auto pose = Quaternion(seed, trial), joint_pose = Quaternion(seed, trial + 1),
               additional = Quaternion(seed, trial + 2);
    std::memcpy(memory.Translate((layout.animator + 0x10) & ~15u, 16), pose.data(), 16);
    // Scalar fields last: a low pose alignment physically overlaps its own
    // rounded rotation quadword, and the joint pointer must stay valid.
    std::memcpy(memory.Translate(layout.animator, 8), &flags, 8);
    memory.Write32(layout.animator + 4, layout.joint | (trial % 2 ? layout.segment : 0));
    std::memcpy(memory.Translate((layout.joint + 0x10) & ~15u, 16), joint_pose.data(), 16);
    std::memcpy(memory.Translate((layout.joint + 0x40) & ~15u, 16), additional.data(), 16);
    memory.bytes[layout.joint + 0x51] = static_cast<uint8_t>(layout.index);
    if (trial % 4 == 0)
        for (unsigned lane = 0; lane < 3; ++lane) memory.Write32(Library::kDefaultScale + lane * 4, 0x3f800000u);
    // Animation data: settings table, static samples and the two frame streams.
    const uint32_t settings_base = 0x340, statics_base = 0x400, current_base = 0x600, next_base = 0x800;
    memory.Write32(layout.data + 4, settings_base | (trial % 5 ? 0 : layout.segment));
    memory.Write32(layout.data + 8, statics_base);
    memory.Write32(layout.data + 0xc, current_base);
    memory.Write32(layout.data + 0x10, next_base | (trial % 7 ? 0 : layout.segment));
    const uint32_t settings = settings_base + layout.index * 8;
    const uint16_t channels = static_cast<uint16_t>((trial / 3) % 16);
    const uint16_t setting_flags = static_cast<uint16_t>((Next(seed) & 0xc0ffu) | (channels << 8) |
        ((trial / 5) % 2 ? 0x1000u : 0u) | ((trial / 7) % 2 ? 0x2000u : 0u));
    const uint16_t statics = static_cast<uint16_t>(trial % 11 == 0 ? 0x1ffu : trial % 11 == 1 ? 0u : Next(seed));
    const uint16_t statics_index = static_cast<uint16_t>(trial % 50), frame_index = static_cast<uint16_t>((trial * 7) % 100);
    std::memcpy(memory.Translate(settings, 2), &setting_flags, 2);
    std::memcpy(memory.Translate(settings + 2, 2), &statics, 2);
    std::memcpy(memory.Translate(settings + 4, 2), &statics_index, 2);
    std::memcpy(memory.Translate(settings + 6, 2), &frame_index, 2);
    if (trial % 17 == 0) {
        // Half-turn ties and extremes for the angle decoder's wrap rule.
        constexpr std::array<int16_t, 6> pins{0x7fff, -0x8000, 0x800, -0x800, 0, 1};
        for (unsigned i = 0; i < 18; ++i) {
            const int16_t sample = pins[Next(seed) % pins.size()];
            std::memcpy(memory.Translate(current_base + frame_index * 2 + i * 2, 2), &sample, 2);
            std::memcpy(memory.Translate(next_base + frame_index * 2 + i * 2, 2), &sample, 2);
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
    for (unsigned reg = 0; reg < 2; ++reg)
        for (unsigned lane = 0; lane < 4; ++lane)
            if (actual.vuRegs.VF[reg].I[lane] != expected.vuRegs.VF[reg].I[lane])
                std::fprintf(stderr, "trial=%u VF%u[%u] selected=%08x control=%08x\n", trial, reg, lane,
                    actual.vuRegs.VF[reg].I[lane], expected.vuRegs.VF[reg].I[lane]);
}
void MemoryDifference(const Oracle::Memory& actual, const Oracle::Memory& expected, unsigned trial) {
    for (size_t offset = 0; offset < actual.bytes.size(); ++offset)
        if (actual.bytes[offset] != expected.bytes[offset]) {
            std::fprintf(stderr, "trial=%u memory+%zx selected=%02x control=%02x\n", trial, offset,
                actual.bytes[offset], expected.bytes[offset]);
            return;
        }
}
Library::Body Install(Library& library, Oracle::Memory& memory, VuUnit& unit, bool selected) {
    Library::Body installed;
    Check(library.Install([&](uint32_t address) -> Library::Body {
        Check(address == Library::kRotate || address == Library::kTransform || address == Library::kAnimate,
            "capture exact source joint entries");
        if (address != Library::kAnimate)
            return [](CpuContext&, uint32_t) { Check(false, "animate cases never dispatch the other joint entries"); };
        return [&](CpuContext& context, uint32_t entered) { Oracle::Invoke(context, memory, unit, entered); };
    }, [&](uint32_t address, Library::Body body) {
        Check(address == Library::kRotate || address == Library::kTransform || address == Library::kAnimate,
            "install exact source joint entries");
        if (address == Library::kAnimate) installed = std::move(body);
        return true;
    }, selected), "joint animate adapter installs");
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
    const auto dispatch = trial % 2 ? Library::kAnimate : 0;
    const auto model_before = Oracle::Counts();
    artifacts.Begin({Library::kAnimate, dispatch, 0, trial, 0, selected}, expected, expected_memory.bytes.data());
    artifacts.RunModel([&] { Oracle::Invoke(expected, expected_memory, expected_unit, dispatch); });
    artifacts.ModelOutput(expected, expected_memory.bytes.data());
    const auto model_after = Oracle::Counts();
    unsigned delegated = 0;
    Library library(actual_memory, [&](CpuContext& context, uint32_t address) {
        Check(address == Library::kReadJointAngle, "the adapter delegates only the angle decoder");
        ++delegated;
        Oracle::InvokeDecoder(context, actual_memory, address);
    });
    const auto installed = Install(library, actual_memory, actual_unit, selected);
    const auto before = Oracle::Counts();
    const int mode = std::fegetround();
    artifacts.CheckActualInput(actual, actual_memory.bytes.data());
    observations.Input(selected, dispatch, actual, actual_memory.bytes.data());
    const bool blend = actual.fpuRegs.fpr[12].f < 1.0f;
    const bool turned = (actual_memory.Read32(0x340 + layout.index * 8) & 0x1000u) != 0;
    installed(actual, dispatch);
    const auto after = Oracle::Counts();
    if (artifacts.Enabled()) artifacts.ActualOutput(actual, actual_memory.bytes.data(), library.Census());
    observations.Output(selected, actual, actual_memory.bytes.data());
    const bool same_memory = actual_memory.bytes == expected_memory.bytes;
    const bool same = same_memory &&
        (selected ? observations.Equal(actual, expected) : std::memcmp(&actual, &expected, sizeof(actual)) == 0);
    Check(same, "complete scalar ABI, memory and source frame rotation/movement match actual original");
    if (!same) { Difference(actual, expected, trial); if (!same_memory) MemoryDifference(actual_memory, expected_memory, trial); }
    const auto counts = library.Counts();
    const auto originals = selected ? 0u : 1u;
    Check(after.generated_bodies - before.generated_bodies == originals &&
        counts.generated_body_animate == originals && counts.animations == (selected ? 1u : 0u) &&
        counts.blended_animations == (selected && blend ? 1u : 0u) &&
        counts.angle_reads == (selected ? 3u : 0u) && delegated == (selected ? 3u : 0u),
        "actual original body executions, owned frames and decoder delegations match selection and weight");
    Check(model_after.decoder_calls - model_before.decoder_calls == 3 &&
        model_after.euler_programs - model_before.euler_programs == 1 &&
        model_after.turn_programs - model_before.turn_programs == (turned ? 1u : 0u) &&
        model_after.interpolate_programs - model_before.interpolate_programs == (blend ? 1u : 0u) &&
        model_after.scale_calls - model_before.scale_calls == (blend ? 1u : 0u) &&
        model_after.translate_calls - model_before.translate_calls == (blend ? 1u : 0u),
        "direct original decodes three angles, blends the frame and composes exactly as the source says");
    Check(after.decoder_calls - before.decoder_calls == 3 &&
        after.euler_programs - before.euler_programs == originals &&
        after.turn_programs - before.turn_programs == (turned ? originals : 0u) &&
        after.interpolate_programs - before.interpolate_programs == (blend ? originals : 0u) &&
        after.scale_calls - before.scale_calls == (blend ? originals : 0u) &&
        after.translate_calls - before.translate_calls == (blend ? originals : 0u),
        "selected animate performs no original program or pose-blend call; control performs the source calls");
    Check(counts.generated_interiors == 0 && counts.unsupported_inputs == 0 && counts.rotations == 0 &&
        counts.transforms == 0 && std::fegetround() == mode,
        "valid source entry requires no unsupported inputs and restores rounding");
    ++cases;
    if (selected) (blend ? blended : published) += 1;
    return same;
}
void Interiors() {
    constexpr std::array<uint32_t, 25> interiors{
        0x293fa0, 0x293fb0, 0x294004, 0x29402c, 0x294054, 0x294068, 0x29407c, 0x294088,
        0x2940b0, 0x2940c0, 0x294114, 0x29413c, 0x294164, 0x294178, 0x294184, 0x2941c0,
        0x2941d0, 0x2941ec, 0x294208, 0x294214, 0x29422c, 0x29424c, 0x29425c, 0x29426c,
        0x2942cc};
    for (bool selected : {false, true}) {
        Oracle::Memory memory;
        VuUnit unit(0);
        unit.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
        MathLibraryDispatchFixture::Reset();
        MathLibraryDispatchFixture::Bind(Library::kAnimate,
            [&](CpuContext& context, uint32_t entry) { Oracle::Invoke(context, memory, unit, entry); });
        for (uint32_t other : {Library::kRotate, Library::kTransform})
            MathLibraryDispatchFixture::Bind(other,
                [](CpuContext&, uint32_t) { Check(false, "animate interiors never dispatch the other entries"); });
        Library library(memory);
        Check(library.Install(DispatchTable::Capture, DispatchTable::Install, selected),
            "joint actual dispatch install");
        uint32_t seed = 0x414e494d;
        for (uint32_t entry : interiors) {
            CpuContext actual{};
            Layout layout;
            Fixture(actual, memory, seed, static_cast<unsigned>(entry % 61), layout);
            // A plausible mid-body frame: reader, slot pointers, loop counters
            // at their last iteration, callee-saved pose/data pointers.
            auto& regs = actual.cpuRegs.GPR.r;
            const uint32_t base = layout.stack - 0xf0;
            regs[29].SD[0] = static_cast<int32_t>(base);
            regs[19].UD[0] = layout.animator;
            regs[20].SD[0] = static_cast<int32_t>(0x340 + layout.index * 8);
            regs[9].SD[0] = static_cast<int32_t>(base);
            regs[12].SD[0] = regs[17].SD[0] = static_cast<int32_t>(base + 0x40);
            regs[23].SD[0] = regs[10].SD[0] = static_cast<int32_t>(base + 0x10);
            regs[8].SD[0] = static_cast<int32_t>(base + 0x20);
            regs[16].SD[0] = static_cast<int32_t>(base + 0x30);
            regs[22].SD[0] = regs[4].SD[0] = static_cast<int32_t>(base + 0x50);
            regs[21].SD[0] = regs[5].SD[0] = static_cast<int32_t>(base + 0x60);
            regs[6].SD[0] = static_cast<int32_t>(base);
            regs[2].SD[0] = 0x400;
            regs[11].SD[0] = regs[18].SD[0] = regs[7].SD[0] = 0;
            // Flag-carrying temporaries of the later blocks hold the pose flags;
            // the earlier blocks hold the source's constant eight.
            std::memcpy(&regs[3].UD[0], memory.Translate(layout.animator, 8), 8);
            if (entry >= 0x2941ec) regs[4].UD[0] = regs[3].UD[0];
            else if (entry != 0x29426c) regs[3].SD[0] = 8;
            actual.fpuRegs.fpr[1].UL = 0x39800000u;
            const uint16_t statics = static_cast<uint16_t>(entry), mask = 0x1ff;
            std::memcpy(memory.Translate(base, 2), &statics, 2);
            std::memcpy(memory.Translate(base + 2, 2), &mask, 2);
            memory.Write32(base + 4, 0x400 + entry % 32 * 2);
            memory.Write32(base + 8, 0x600 + entry % 64 * 2);
            memory.Write32(base + 0xc, 0x800 + entry % 64 * 2);
            CpuContext expected = actual;
            auto expected_memory = memory;
            VuUnit expected_unit(0);
            expected_unit.WriteMicroMem(0, math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
            Oracle::Invoke(expected, expected_memory, expected_unit, entry);
            Check(DispatchTable::CanResumeAt(Library::kAnimate, entry) &&
                !DispatchTable::CanResumeAt(Library::kAnimate, entry + 1),
                "only reviewed exact original interiors keep replay proof");
            Check(DispatchTable::InvokeAt(Library::kAnimate, entry, actual) &&
                std::memcmp(&actual, &expected, sizeof(actual)) == 0 && memory.bytes == expected_memory.bytes,
                "joint animate interior resumes actual frame without full-entry replay");
        }
        Check(!selected || !DispatchTable::CanResumeAt(Library::kAnimate, 0),
            "selected full source entry gains no generated replay proof");
        const auto counts = library.Counts();
        Check(counts.generated_body_animate == interiors.size() &&
            counts.generated_interiors == (selected ? interiors.size() : 0u) && counts.animations == 0 &&
            counts.angle_reads == 0,
            "interior delegation counts every actual original body and delegates no decoder call");
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
        originals == after.unsupported_inputs && after.refused_animate == counts.refused_animate + 1 &&
        after.unsupported_inputs == counts.unsupported_inputs + 1 && after.animations == counts.animations &&
        library.LastRefusal() == reason, message);
}
// The title's stack lives in the mirrored scratchpad (every anchor's boundary
// `$sp` is 0x70004220, above the 16 KB end): AnimateJoint's 0xf0-byte frame and
// the decoder frame below it may straddle the 0x70004000 mirror seam, where one
// host translation of the whole frame fails while the generated quad accesses
// go through the mirror (engine/ee/mem/scratchpad.h: offset = address & 0x3fff).
class MirroredMemory final : public ps2r::ipu::GuestMemory {
public:
    std::array<uint8_t, Oracle::Memory::kLow + Oracle::Memory::kWindowSize> bytes{};
    std::array<uint8_t, 0x4000> scratchpad{};
    uint8_t* Translate(uint32_t address, size_t length) override {
        if ((address & 0x70000000u) == 0x70000000u) {
            const uint32_t offset = address & 0x3fffu;
            return length <= scratchpad.size() - offset ? scratchpad.data() + offset : nullptr;
        }
        Oracle::Memory low;
        auto* host = low.Translate(address, length);
        return host ? bytes.data() + (host - low.bytes.data()) : nullptr;
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
    std::array<decltype(MirroredMemory::bytes), 2> rams{};
    unsigned originals = 0, decoded = 0;
    for (unsigned straddling = 0; straddling < 2; ++straddling) {
        MirroredMemory memory;
        // A decoder stand-in with the source ABI effects the adapter consumes:
        // one angle word into both outputs, the live cursors advanced.
        Library library(memory, [&](CpuContext& context, uint32_t address) {
            Check(address == Library::kReadJointAngle, "seam fixture delegates to the angle decoder entry");
            ++decoded;
            auto& regs = context.cpuRegs.GPR.r;
            const uint32_t reader = regs[4].UL[0];
            memory.Write32(regs[5].UL[0], 0x3e000000u + decoded % 3 * 0x800000u);
            memory.Write32(regs[6].UL[0], 0x3e400000u + decoded % 3 * 0x800000u);
            memory.Write32(reader + 8, memory.Read32(reader + 8) + 2);
            memory.Write32(reader + 0xc, memory.Read32(reader + 0xc) + 2);
        });
        Library::Body retained;
        Check(library.Install([&](uint32_t) -> Library::Body {
            return [&](CpuContext&, uint32_t) { ++originals; };
        }, [&](uint32_t address, Library::Body body) {
            if (address == Library::kAnimate) retained = std::move(body);
            return true;
        }, true), "animate mirror seam fixture installs");
        CpuContext context{};
        context.vuRegs.VF[0].F[3] = 1;
        context.cpuRegs.GPR.r[4].UL[0] = 0x100;
        context.cpuRegs.GPR.r[5].UL[0] = 0x300;
        context.cpuRegs.GPR.r[29].UL[0] = straddling ? 0x70004080u : 0x70001000u;
        context.fpuRegs.fpr[12].f = 0.5f;
        context.fpuRegs.fpr[13].f = 0.25f;
        memory.Write32(0x104, 0x200);
        memory.bytes[0x251] = 1;
        memory.Write32(0x304, 0x340); memory.Write32(0x308, 0x400);
        memory.Write32(0x30c, 0x600); memory.Write32(0x310, 0x800);
        memory.bytes[0x349] = 0x9;  // Nine channels per track, all live.
        for (uint32_t sample = 0; sample < 0x40; sample += 2) {
            memory.bytes[0x600 + sample] = static_cast<uint8_t>(sample * 3);
            memory.bytes[0x800 + sample] = static_cast<uint8_t>(sample * 5 + 1);
        }
        for (unsigned lane = 0; lane < 3; ++lane) memory.Write32(Library::kDefaultScale + lane * 4, 0x3f800000u);
        retained(context, 0);
        const auto counts = library.Counts();
        Check(counts.animations == 1 && counts.blended_animations == 1 && counts.angle_reads == 3 &&
            counts.refused_animate == 0 && counts.unsupported_inputs == 0 && counts.generated_body_animate == 0 &&
            originals == 0 && decoded == 3 * (straddling + 1),
            straddling ? "animation whose frame straddles the scratchpad mirror seam is owned, not refused"
                       : "animation with a scratchpad frame is owned");
        results[straddling] = context;
        rams[straddling] = memory.bytes;
    }
    // Scalar registers legitimately carry frame addresses; the source-visible
    // results are the vector/FPU registers and the animator pose in memory.
    Check(std::memcmp(&results[0].vuRegs, &results[1].vuRegs, sizeof(VUregs)) == 0 &&
        std::memcmp(&results[0].fpuRegs, &results[1].fpuRegs, sizeof(FPUregs)) == 0 && rams[0] == rams[1],
        "seam-straddling animation publishes the same vector results and animator pose");
}
void FailurePins() {
    Oracle::Memory memory;
    for (bool throwing : {false, true}) {
        std::array<Library::Body, 3> retained;
        unsigned attempts = 0, originals = 0;
        {
            Library library(memory);
            bool refused = false;
            try {
                refused = !library.Install([&](uint32_t) -> Library::Body {
                    return [&](CpuContext&, uint32_t) { ++originals; };
                }, [&](uint32_t address, Library::Body body) {
                    // The third (animate) publication fails; every rollback is rejected.
                    if (attempts++ >= 3) throw std::runtime_error("rollback rejected");
                    retained[address == Library::kTransform ? 1 : address == Library::kAnimate ? 2 : 0] = std::move(body);
                    if (address != Library::kAnimate) return true;
                    CpuContext context{};
                    retained[2](context, 0);
                    if (throwing) throw std::runtime_error("publication failed");
                    return false;
                }, true);
            } catch (const std::runtime_error& error) {
                refused = std::string(error.what()) == "publication failed";
            }
            Check(refused && originals == 1 && attempts == 6 && library.Counts().animations == 0,
                "failed third publication is inactive and restores the first two even when rollback throws");
        }
        for (auto& body : retained) { CpuContext context{}; body(context, 0); }
        Check(originals == 4, "all three failed joint callbacks survive quiescent owner destruction");
    }
    Library::Body retained;
    unsigned originals = 0, decoded = 0;
    bool corrupt = false;
    {
        Library library(memory, [&](CpuContext& context, uint32_t address) {
            ++decoded;
            if (corrupt) memory.Write32(context.cpuRegs.GPR.r[29].UL[0] + 8, 0x1000);  // Unmapped cursor.
            else Oracle::InvokeDecoder(context, memory, address);
        });
        Check(library.Install([&](uint32_t) -> Library::Body {
            return [&](CpuContext&, uint32_t) { ++originals; };
        }, [&](uint32_t address, Library::Body body) {
            if (address == Library::kAnimate) retained = std::move(body);
            return true;
        }, true), "joint animate lifetime fixture installs");
        const auto valid = [&](CpuContext& context) {
            context = {};
            context.vuRegs.VF[0].F[3] = 1;
            context.cpuRegs.GPR.r[4].UL[0] = 0x100;
            context.cpuRegs.GPR.r[5].UL[0] = 0x300;
            context.cpuRegs.GPR.r[29].UL[0] = 0xf00;
            context.fpuRegs.fpr[12].f = 0.5f;
            std::memset(memory.Translate(0x100, 0x50), 0, 0x50);
            memory.Write32(0x104, 0x200);
            memory.bytes[0x251] = 1;
            memory.Write32(0x304, 0x340); memory.Write32(0x308, 0x400);
            memory.Write32(0x30c, 0x600); memory.Write32(0x310, 0x800);
            std::memset(memory.Translate(0x348, 8), 0, 8);
            memory.bytes[0x349] = 0x3;  // Three channels per track.
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
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0xf08; refuse(R::Frame, "unaligned frame declines unchanged");
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0x110; refuse(R::Frame, "frame below the decoder frame declines");
        valid(context); context.cpuRegs.GPR.r[29].UL[0] = 0x1f0; refuse(R::Frame, "frame aliasing the animator declines");
        valid(context); context.cpuRegs.GPR.r[4].UL[0] = 0xffc; refuse(R::Objects, "unmapped animator declines unchanged");
        valid(context); context.cpuRegs.GPR.r[5].UL[0] = 0xff0; refuse(R::Objects, "unmapped animation data declines unchanged");
        valid(context); memory.Write32(0x104, 0xffb0); refuse(R::Objects, "unmapped joint declines unchanged");
        valid(context); memory.Write32(0x304, 0xffc); refuse(R::Objects, "unmapped settings decline unchanged");
        valid(context); memory.Write32(0x30c, 0x1000); refuse(R::Objects, "unmapped channel samples decline unchanged");
        valid(context); memory.Write32(0x308, 0x1000); memory.bytes[0x34a] = 1; refuse(R::Objects, "unmapped static samples decline unchanged");
        auto counts = library.Counts();
        Check(counts.unsupported_inputs == refusals && refusals == 12 && decoded == 0 &&
            counts.refused_animate == 12 && counts.refusal_state == 3 && counts.refusal_frame == 3 &&
            counts.refusal_objects == 6 && counts.refusal_channels == 0 && counts.faults == 0 &&
            counts.generated_body_animate == 12 && originals == 12,
            "invalid source frames/state have an actual per-reason refusal count before any delegation");
        // A delegated decoder that leaves an unreadable cursor is found after
        // the three delegations: every write so far is in the dead callee frame,
        // so the entry context is restored and the call declines to the original
        // (the delegations stay counted at the decoder's dispatch).
        valid(context);
        corrupt = true;
        const auto entry = context;
        bool threw = false;
        try { retained(context, 0); }
        catch (...) { threw = true; }
        counts = library.Counts();
        Check(!threw && std::memcmp(&context, &entry, sizeof(context)) == 0 && decoded == 3 && originals == 13 &&
            counts.unsupported_inputs == 13 && counts.refused_animate == 13 && counts.refusal_channels == 1 &&
            counts.animations == 0 && counts.angle_reads == 3 && counts.generated_body_animate == 13 &&
            library.LastRefusal() == R::Channels,
            "unreadable scale channel after delegation restores the entry context and declines to the original");
        corrupt = false;
        originals = 0;
    }
    CpuContext context{};
    retained(context, 0);
    Check(originals == 1, "successful joint animate callback safely forwards after quiescent owner teardown");
    MirrorSeamPins();
}
}

int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryJointAnimateOracle", Oracle::SourceDigest(),
        Oracle::Memory{}.bytes.size(), 0);
    artifacts.Fixture("standard_program", math_joint_oracle::kImage.data(), math_joint_oracle::kImage.size());
    MathLibraryJointObservations observations(argc == 3 ? argv[2] : nullptr, Oracle::Memory{}.bytes.size(),
        MathLibraryJointObservations::Scope::Animate);
    const int previous = std::fegetround();
    Interiors();
    FailurePins();
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "set joint animate rounding");
        uint32_t seed = 0x414e4a54;
        for (unsigned trial = 0; trial < 1500; ++trial) {
            auto control_seed = seed;
            if (!Compare(trial, seed, true, artifacts, observations) ||
                !Compare(trial, control_seed, false, artifacts, observations)) return 1;
            Check(seed == control_seed, "actual control and selected consume the same fixture stream");
        }
    }
    std::fesetround(previous);
    if (!failures) { observations.Finish(); artifacts.Finish(); }
    const auto counts = Oracle::Counts();
    std::printf("MathLibraryJointAnimateOracle checks=%u failures=%u cases=%llu published=%llu blended=%llu "
                "actual_bodies=%llu actual_decoder_calls=%llu actual_scale_calls=%llu actual_translate_calls=%llu "
                "actual_euler_programs=%llu actual_turn_programs=%llu actual_interpolate_programs=%llu\n",
        checks, failures, static_cast<unsigned long long>(cases), static_cast<unsigned long long>(published),
        static_cast<unsigned long long>(blended), static_cast<unsigned long long>(counts.generated_bodies),
        static_cast<unsigned long long>(counts.decoder_calls), static_cast<unsigned long long>(counts.scale_calls),
        static_cast<unsigned long long>(counts.translate_calls), static_cast<unsigned long long>(counts.euler_programs),
        static_cast<unsigned long long>(counts.turn_programs),
        static_cast<unsigned long long>(counts.interpolate_programs));
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryJointAnimateOracle: %s\n", error.what());
        return 1;
    }
}
