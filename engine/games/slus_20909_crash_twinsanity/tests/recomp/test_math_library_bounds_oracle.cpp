#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_bounds_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_dispatch_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle_artifacts.h"
#include "games/slus_20909_crash_twinsanity/hle/math/bounds_math.h"

#include <cfenv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

uint8_t* MathLibraryBoundsOracle::Memory::Translate(uint32_t address, size_t length) {
    address &= 0x1fffffffu;
    if (address > bytes.size() || length > bytes.size() - address) return nullptr;
    return bytes.data() + address;
}
uint32_t MathLibraryBoundsOracle::Memory::Read32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, Translate(address, 4), 4);
    return value;
}
void MathLibraryBoundsOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, 4), &value, 4);
}

namespace {
using Oracle = MathLibraryBoundsOracle;
using Math = title::BoundsMath;
unsigned checks = 0, failures = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value && failures++ < 16) std::fprintf(stderr, "%s\n", message);
}
uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
uint32_t Bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, 4);
    return result;
}
float FromBits(uint32_t value) {
    float result;
    std::memcpy(&result, &value, 4);
    return result;
}
void Fixture(CpuContext& context, Oracle::Memory& memory, uint32_t& seed, unsigned trial, size_t op) {
    for (size_t offset = 0; offset < sizeof(context); offset += 4) {
        const uint32_t word = Next(seed);
        std::memcpy(reinterpret_cast<uint8_t*>(&context) + offset, &word, 4);
    }
    for (size_t offset = 0; offset < memory.bytes.size(); offset += 4) {
        uint32_t word = Next(seed);
        if (trial % 3 == 0) word = Bits(static_cast<float>(static_cast<int32_t>(word)) / 65536.0f);
        memory.Write32(static_cast<uint32_t>(offset), word);
    }
    const uint32_t segment = std::array<uint32_t, 3>{0, 0x80000000, 0xa0000000}[trial % 3];
    const uint32_t bias = trial % 16;
    const uint32_t first = segment | (0x100u + bias);
    const uint32_t second = segment | (0x180u + bias);
    const uint32_t third = segment | (0x200u + bias);
    const uint32_t matrix = segment | (0xa00u + bias);
    const uint32_t output = std::array<uint32_t, 6>{
        segment | (0xb00u + bias), first, first + 16, second, matrix, matrix + 32}[trial % 6];
    auto& regs = context.cpuRegs.GPR.r;
    if (op == 0) {
        regs[4].UL[0] = output; regs[5].UL[0] = first;
        regs[6].UL[0] = second; regs[7].UL[0] = third;
    } else {
        const int32_t count = std::array<int32_t, 16>{
            -100, -2, -1, 0, 1, 2, 3, 4, 5, 8, 15, 16, 31, 32, 63, 127}[trial % 16];
        regs[4].UL[0] = first; regs[5].SL[0] = count;
        regs[6].UL[0] = output; regs[7].UL[0] = trial % 11 == 0 ? first : matrix;
    }
    if (trial == 5) {
        // The OLD raw-payload pin is also an actual exported oracle case,
        // including a two-point min/max and the triangle's retained w lanes.
        const uint32_t raw[]{0x0000807f, 0x8000807f, 0x7fa01234, 0xff801234};
        for (uint32_t address : {first, first + 16, second, third})
            std::memcpy(memory.Translate(address & ~15u, sizeof(raw)), raw, sizeof(raw));
        context.vuRegs.VF[11].I[3] = 0x7fa00123;
        context.vuRegs.VF[12].I[3] = 0xff801234;
    }
}
void Difference(const CpuContext& actual, const CpuContext& control, const Oracle::Memory& a,
                const Oracle::Memory& b, uint32_t entry, unsigned trial, int mode) {
    for (size_t offset = 0; offset < sizeof(actual); offset += 4) {
        uint32_t x, y;
        std::memcpy(&x, reinterpret_cast<const uint8_t*>(&actual) + offset, 4);
        std::memcpy(&y, reinterpret_cast<const uint8_t*>(&control) + offset, 4);
        if (x != y) {
            std::fprintf(stderr, "entry=%x trial=%u mode=%d context+%zu actual=%08x control=%08x\n",
                entry, trial, mode, offset, x, y);
            break;
        }
    }
    for (size_t offset = 0; offset < a.bytes.size(); ++offset)
        if (a.bytes[offset] != b.bytes[offset]) {
            std::fprintf(stderr, "entry=%x trial=%u memory+%zu actual=%02x control=%02x\n",
                entry, trial, offset, a.bytes[offset], b.bytes[offset]);
            break;
        }
}
bool Compare(size_t op, unsigned trial, uint32_t& seed, bool selected,
             MathLibraryOracleArtifacts& artifacts) {
    const auto entry = Math::Entries()[op];
    const int mode = std::fegetround();
    Oracle::Memory expected_memory, actual_memory;
    CpuContext expected{}, actual{};
    Fixture(expected, expected_memory, seed, trial, op);
    actual_memory.bytes = expected_memory.bytes;
    std::memcpy(&actual, &expected, sizeof(actual));
    const uint32_t dispatch = trial % 2 ? entry.address : 0;
    artifacts.Begin({entry.address, dispatch, static_cast<uint32_t>(op), trial, 0, selected},
        expected, expected_memory.bytes.data());
    artifacts.RunModel([&] { Oracle::Invoke(entry.address, expected, expected_memory); });
    artifacts.ModelOutput(expected, expected_memory.bytes.data());
    Math math(actual_memory);
    std::map<uint32_t, Math::Body> installed;
    Check(math.Install([&](uint32_t address) -> Math::Body {
        return [&, address](CpuContext& context, uint32_t entered) {
            Oracle::Invoke(address, context, actual_memory, entered);
        };
    }, [&](uint32_t address, Math::Body body) {
        return installed.emplace(address, std::move(body)).second;
    }, selected), "bounds group installs");
    artifacts.CheckActualInput(actual, actual_memory.bytes.data());
    const auto before = Oracle::Counts();
    installed.at(entry.address)(actual, dispatch);
    const auto after = Oracle::Counts();
    if (artifacts.Enabled()) artifacts.ActualOutput(actual, actual_memory.bytes.data(), math.Census());
    const bool equal = expected_memory.bytes == actual_memory.bytes &&
        std::memcmp(&actual, &expected, sizeof(actual)) == 0;
    Check(equal, "whole bounds context/memory differs from actual NEW");
    if (!equal) Difference(actual, expected, actual_memory, expected_memory, entry.address, trial, mode);
    const auto counts = math.Counts();
    for (size_t index = 0; index < Math::Entries().size(); ++index) {
        const uint64_t actual_original = !selected && index == op ? 1 : 0;
        Check(after[index] - before[index] == actual_original &&
            counts.generated_body_calls[index] == actual_original &&
            counts.owned_calls[index] == (selected && index == op ? 1u : 0u),
            "actual generated execution and owned work are selection-aware");
    }
    Check(counts.generated_interiors == 0 && counts.unsupported_inputs == 0,
        "complete bounds cases need no unsupported or interior path");
    Check(std::fegetround() == mode, "bounds preserves caller rounding");
    return equal;
}
void ValuePins() {
    const Math::Vector raw{FromBits(0x807f), FromBits(0x8000807f), FromBits(0x7fa01234), FromBits(0xff801234)};
    const auto copy = Math::Points(&raw, 1);
    Check(std::memcmp(copy.box.minimum.data(), raw.data(), 16) == 0 &&
        std::memcmp(copy.box.maximum.data(), raw.data(), 16) == 0,
        "R109b raw payload, denormal and special words survive bounds copies");
    const Math::Vector points[]{raw, raw};
    const auto pair = Math::Points(points, 2);
    Check(std::memcmp(pair.box.minimum.data(), raw.data(), 16) == 0 &&
        std::memcmp(pair.box.maximum.data(), raw.data(), 16) == 0,
        "R109b MIN/MAX selection never clamps raw payloads");
    const Math::Vector zeros[]{{FromBits(0x80000000), 0, 1, 9}, {0, FromBits(0x80000000), 2, 10}};
    const auto tied = Math::Points(zeros, 2);
    Check(Bits(tied.box.minimum[0]) == 0x80000000 && Bits(tied.box.maximum[0]) == 0x80000000 &&
        Bits(tied.box.minimum[1]) == 0 && Bits(tied.box.maximum[1]) == 0,
        "equal signed-zero operands keep source operand order");
    const auto triangle = Math::Triangle({3, -4, 8, 99}, {-2, 5, 1, 88}, {0, 2, 7, 77},
        FromBits(0x7fa01234), FromBits(0xff801234));
    Check(triangle.minimum[0] == -2 && triangle.minimum[1] == -4 && triangle.minimum[2] == 1 &&
        triangle.maximum[0] == 3 && triangle.maximum[1] == 5 && triangle.maximum[2] == 8 &&
        Bits(triangle.minimum[3]) == 0x7fa01234 && Bits(triangle.maximum[3]) == 0xff801234,
        "triangle bounds preserve both existing w lanes");
    Oracle::Memory memory;
    Math math(memory);
    for (auto address : {0x201428u, 0x201460u}) {
        for (int32_t count : {-1, 0, 1}) {
            CpuContext context{};
            context.cpuRegs.GPR.r[4].UL[0] = 0x100;
            context.cpuRegs.GPR.r[5].SL[0] = count;
            context.cpuRegs.GPR.r[6].UL[0] = 0x200;
            context.cpuRegs.GPR.r[7].UL[0] = 0x300;
            const Math::Matrix identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
            const Math::Vector first{2, 3, 4, 7}, next{8, 9, 10, 11};
            std::memcpy(memory.bytes.data() + 0x100, first.data(), 16);
            std::memcpy(memory.bytes.data() + 0x110, next.data(), 16);
            std::memcpy(memory.bytes.data() + 0x300, identity.data(), 64);
            Check(math.Invoke(address, context), "source nonpositive/single-point count remains initial point");
            Check(context.cpuRegs.GPR.r[4].UL[0] == 0x110 && context.cpuRegs.GPR.r[5].SD[0] == count - 1 &&
                std::memcmp(context.vuRegs.VF[3].F, next.data(), 16) == 0 &&
                std::memcmp(memory.bytes.data() + 0x200, first.data(), 16) == 0,
                "count<=1 preserves generated look-ahead, pointer/count result and first-point w");
        }
    }
    CpuContext bad{};
    bad.cpuRegs.GPR.r[4].UL[0] = 0x100; bad.cpuRegs.GPR.r[6].UL[0] = 0x200;
    bad.cpuRegs.GPR.r[7].UL[0] = 0x300;
    for (int32_t count : {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()}) {
        bad.cpuRegs.GPR.r[5].SL[0] = count;
        const auto before = bad;
        const auto bytes = memory.bytes;
        Check(!math.Invoke(0x201460, bad) && std::memcmp(&bad, &before, sizeof(bad)) == 0 &&
            bytes == memory.bytes, "invalid huge/wrapped count leaves all state unchanged");
    }
    bad.cpuRegs.GPR.r[5].SL[0] = 1;
    bad.cpuRegs.GPR.r[4].UL[0] = 4080;
    Check(!math.Invoke(0x201428, bad), "one-point input still requires mapped look-ahead");
}
void ContinuationPins() {
    Oracle::Memory memory;
    for (bool selected : {false, true}) {
        MathLibraryDispatchFixture::Reset();
        for (const auto& entry : Math::Entries())
            MathLibraryDispatchFixture::Bind(entry.address, [&, address = entry.address]
                (CpuContext& context, uint32_t dispatch) { Oracle::Invoke(address, context, memory, dispatch); });
        Math math(memory);
        Check(math.Install(DispatchTable::Capture, DispatchTable::Install, selected), "bounds actual dispatch install");
        Check(!math.Install(DispatchTable::Capture, DispatchTable::Install, selected), "bounds duplicate install refuses");
        for (const uint32_t function : {0x201428u, 0x201460u}) {
            const auto entry = function == 0x201428 ? 0x201454u : 0x2014ccu;
            CpuContext actual{}, expected{};
            actual.cpuRegs.GPR.r[6].UL[0] = 0x200;
            actual.vuRegs.VF[1].F[0] = 8; actual.vuRegs.VF[2].F[1] = 9;
            expected = actual;
            auto expected_memory = memory;
            Oracle::Invoke(function, expected, expected_memory, entry);
            Check(DispatchTable::CanResumeAt(function, entry) && !DispatchTable::CanResumeAt(function, entry + 1),
                "exact generated epilogue proof remains available in both selections");
            Check(DispatchTable::CanResumeAt(function, 0) == !selected,
                "selected source entry cannot invent original full-entry replay proof");
            Check(DispatchTable::InvokeAt(function, entry, actual) &&
                std::memcmp(&actual, &expected, sizeof(actual)) == 0 && memory.bytes == expected_memory.bytes,
                "actual generated interior resumes without repeating source entry");
        }
        const auto counts = math.Counts();
        Check(counts.generated_body_calls[1] == 1 && counts.generated_body_calls[2] == 1 &&
            counts.generated_interiors == (selected ? 2u : 0u),
            "interior replay records each actual original execution");
    }
    MathLibraryDispatchFixture::Reset();
}
void InstallationPins() {
    Oracle::Memory memory;
    for (size_t failure = 0; failure < Math::Entries().size(); ++failure) {
        unsigned calls = 0;
        Math missing(memory);
        Check(!missing.Install([&](uint32_t address) -> Math::Body {
            if (address == Math::Entries()[failure].address) return {};
            return [](CpuContext&, uint32_t) {};
        }, [&](uint32_t, Math::Body) { ++calls; return true; }, true) && calls == 0,
            "bounds captures whole batch before publication");
        for (bool throwing : {false, true}) {
            std::map<uint32_t, Math::Body> retained;
            unsigned attempts = 0, originals = 0;
            {
                Math math(memory);
                bool refused = false;
                try {
                    refused = !math.Install([&](uint32_t) -> Math::Body {
                        return [&](CpuContext& context, uint32_t address) {
                            ++originals; context.cpuRegs.pc = address;
                        };
                    }, [&](uint32_t address, Math::Body body) {
                        const auto attempt = attempts++;
                        if (attempt > failure) {
                            if (throwing) throw std::runtime_error("rollback failed");
                            return false;
                        }
                        retained[address] = std::move(body);
                        CpuContext context{};
                        const auto before = originals;
                        retained[address](context, address);
                        Check(originals == before + 1 && context.cpuRegs.pc == address,
                            "bounds partial publication remains original");
                        if (attempt == failure && throwing) throw std::runtime_error("publish failed");
                        return attempt != failure;
                    }, true);
                } catch (const std::runtime_error& error) {
                    refused = std::string(error.what()) == "publish failed";
                }
                Check(refused, "bounds failed/throwing publication rejects group");
                for (auto count : math.Counts().owned_calls) Check(count == 0, "failed bounds group owns no work");
            }
            for (const auto& [address, body] : retained) {
                CpuContext context{};
                const auto before = originals;
                body(context, address);
                Check(originals == before + 1 && context.cpuRegs.pc == address,
                    "failed bounds callbacks remain safe after owner destruction");
            }
        }
    }
    std::map<uint32_t, Math::Body> retained;
    unsigned originals = 0;
    {
        Math math(memory);
        Check(math.Install([&](uint32_t) -> Math::Body {
            return [&](CpuContext&, uint32_t) { ++originals; };
        }, [&](uint32_t address, Math::Body body) {
            retained[address] = std::move(body); return true;
        }, true), "bounds lifetime fixture installs");
        CpuContext invalid{};
        invalid.cpuRegs.GPR.r[5].SL[0] = std::numeric_limits<int32_t>::min();
        bool refused = false;
        try { retained.at(0x201428)(invalid, 0x201428); }
        catch (const std::runtime_error&) { refused = true; }
        Check(refused && math.Counts().unsupported_inputs == 1 && originals == 0,
            "invalid selected bounds never silently executes original");
    }
    CpuContext context{};
    retained.at(0x201428)(context, 0x201428);
    Check(originals == 1, "retained bounds callback forwards safely after quiescent teardown");
}
int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryBoundsOracle", Oracle::SourceDigest(),
        Oracle::Memory{}.bytes.size(), 0);
    const int previous = std::fegetround();
    ValuePins();
    ContinuationPins();
    InstallationPins();
    uint64_t cases = 0;
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "set oracle rounding");
        uint32_t seed = 0x424f554e;
        for (size_t op = 0; op < Math::Entries().size(); ++op) {
            for (unsigned trial = 0; trial < 4000; ++trial) {
                auto control_seed = seed;
                if (!Compare(op, trial, seed, true, artifacts) ||
                    !Compare(op, trial, control_seed, false, artifacts)) return 1;
                Check(seed == control_seed, "selected and actual control use identical fixture stream");
                cases += 2;
            }
        }
    }
    std::fesetround(previous);
    const auto counts = Oracle::Counts();
    std::printf("MathLibraryBoundsOracle checks=%u failures=%u cases=%llu actual_new=%llu/%llu/%llu\n",
        checks, failures, static_cast<unsigned long long>(cases),
        static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(counts[1]),
        static_cast<unsigned long long>(counts[2]));
    if (!failures) artifacts.Finish();
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryBoundsOracle: %s\n", error.what());
        return 1;
    }
}
