#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_collision_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle_artifacts.h"

#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"

uint8_t* MathLibraryCollisionOracle::Memory::Translate(uint32_t address, size_t length) {
    address = (address & 0x70000000u) == 0x70000000u
        ? 8192 + (address & 0x3fffu) : address & 0x1fffffffu;
    if (address > bytes.size() || length > bytes.size() - address) {
        std::fprintf(stderr, "collision oracle invalid fixture memory %#x/%zu\n", address, length);
        std::abort();
    }
    return bytes.data() + address;
}

uint32_t MathLibraryCollisionOracle::Memory::Read32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, Translate(address, 4), 4);
    return value;
}

void MathLibraryCollisionOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, 4), &value, 4);
}

namespace {
using Oracle = MathLibraryCollisionOracle;
using Math = title::CollisionMath;

uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

void Seed(Oracle::Memory& memory, CpuContext& context, uint32_t& seed, unsigned trial, unsigned operation) {
    for (auto& byte : memory.bytes) byte = static_cast<uint8_t>(Next(seed));
    for (size_t i = 0; i < sizeof(context); ++i)
        reinterpret_cast<uint8_t*>(&context)[i] = static_cast<uint8_t>(Next(seed));
    // Keep source counted loops finite. Other register bits, including upper
    // halves and unrelated VFs, remain adversarial.
    context.cpuRegs.GPR.r[0].UD[0] = 0;
    constexpr uint32_t mirrors[] = {0, 0x80000000u, 0xa0000000u, 0x70000000u};
    const uint32_t prefix = mirrors[trial % 4];
    const auto address = [&](uint32_t offset) { return prefix | offset | (trial % 16); };
    auto& r = context.cpuRegs.GPR.r;
    r[4].UL[0] = address(0x100);
    r[5].UL[0] = address(0x200);
    r[6].UL[0] = address(0x300);
    r[7].UL[0] = address(trial % 3 == 0 ? 0x100 : 0x600);
    constexpr uint32_t sizes[] = {1, 2, 3, 4, 8, 16};
    const uint32_t count = sizes[(trial / 4) % 6];
    switch (operation) {
        case 0: break;
        case 1: r[4].UL[0] = address(0x600); break;
        case 2: {
            const uint32_t first = sizes[(trial / 16) % 6];
            const uint32_t second = sizes[(trial / 96) % 6];
            // Vary moved/vertex overlap and differences/moved overlap. The
            // request remains separate so its count fields stay valid.
            const uint32_t moved = address(trial % 3 == 0 ? 0x400 : 0x800);
            const uint32_t differences = trial % 5 == 0 ? moved : address(0x1000);
            const uint32_t request[] = {address(0x200), address(0x400), first,
                address(0x300), address(0x600), second, differences, moved};
            for (uint32_t i = 0; i < 8; ++i) memory.Write32(r[4].UL[0] + i * 4, request[i]);
            break;
        }
        case 3: r[5].UL[0] = count; break;
        case 4:
            r[5].UL[0] = count;
            r[6].UL[0] = address(0x500);
            break;
        case 5:
            r[5].UL[0] = address(0x500);
            r[6].UL[0] = address(trial % 3 == 0 ? 0x100 : 0x600);
            break;
        case 6: break;
        case 7: r[5].UL[0] = address(trial % 3 == 0 ? 0x100 : 0x600); break;
        default: std::abort();
    }
}

bool Equal(const CpuContext& actual, const CpuContext& expected,
           const Oracle::Memory& memory, const Oracle::Memory& expected_memory,
           const Math& math, unsigned operation, unsigned expected_calls, unsigned originals, unsigned trial,
           bool selected, const std::array<uint64_t, 8>& expected_entries) {
    const auto counts = math.Counts();
    uint64_t replacements = 0;
    for (const auto count : counts.entries) replacements += count;
    uint64_t generated = 0;
    for (const auto count : counts.generated_body_calls) generated += count;
    const auto zero = std::array<uint64_t, 8>{};
    if (memory.bytes == expected_memory.bytes && std::memcmp(&actual, &expected, sizeof(actual)) == 0 &&
        std::fegetround() == FE_TOWARDZERO && replacements == (selected ? expected_calls : 0) &&
        originals == (selected ? 0 : expected_calls) && generated == originals &&
        counts.generated_continuations == 0 && counts.unsupported_inputs == 0 &&
        counts.entries == (selected ? expected_entries : zero) &&
        counts.generated_body_calls == (selected ? zero : expected_entries)) return true;
    std::fprintf(stderr, "Collision oracle FAIL operation=%u trial=%u calls=%llu expected=%u originals=%u\n",
        operation, trial, static_cast<unsigned long long>(replacements), expected_calls, originals);
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

bool Compare(unsigned operation, unsigned trial, uint32_t& seed, bool sequence, bool selected,
             MathLibraryOracleArtifacts& artifacts) {
    Oracle::Memory memory, expected_memory;
    CpuContext actual, expected;
    Seed(memory, actual, seed, trial, operation);
    std::memcpy(&expected, &actual, sizeof(actual));
    expected_memory.bytes = memory.bytes;
    Math math(memory);
    unsigned originals = 0;
    std::map<uint32_t, Math::Body> table;
    const auto capture = [&](uint32_t address) -> Math::Body {
        return [&, address](CpuContext& context, uint32_t) {
            ++originals;
            Oracle::Invoke(address, context, memory);
        };
    };
    if (!math.Install(capture, [&](uint32_t address, Math::Body body) {
        table[address] = std::move(body);
        return true;
    }, selected)) return false;
    const auto invoke = [&](unsigned index, unsigned step, uint32_t dispatch) {
        const auto& entry = Oracle::Entries()[index];
        artifacts.Begin({entry.address, dispatch, operation, trial, step, selected},
            expected, expected_memory.bytes.data());
        artifacts.RunModel([&] { Oracle::Invoke(entry.address, expected, expected_memory); });
        artifacts.ModelOutput(expected, expected_memory.bytes.data());
        artifacts.CheckActualInput(actual, memory.bytes.data());
        table.at(entry.address)(actual, dispatch);
        if (artifacts.Enabled()) artifacts.ActualOutput(actual, memory.bytes.data(), math.Census());
    };
    invoke(operation, 0, trial % 2 ? Oracle::Entries()[operation].address : 0);
    std::array<uint64_t, 8> expected_entries{};
    expected_entries[operation] = 1;
    if (!Equal(actual, expected, memory, expected_memory, math, operation, 1, originals, trial,
        selected, expected_entries)) return false;
    if (!sequence) return true;
    const uint32_t prefix = actual.cpuRegs.GPR.r[4].UL[0] & 0xffff0000u;
    if (operation == 0) {
        const uint32_t output = actual.cpuRegs.GPR.r[7].UL[0];
        actual.cpuRegs.GPR.r[4].UL[0] = expected.cpuRegs.GPR.r[4].UL[0] = output;
        invoke(1, 1, 0);
        ++expected_entries[1];
        return Equal(actual, expected, memory, expected_memory, math, 1, 2, originals, trial, selected, expected_entries);
    }
    // Load once, then query three axes. The grouped support is owned value
    // data and must survive the query scratch outputs and intervening calls.
    for (unsigned query = 0; query < 3; ++query) {
        const uint32_t axis = prefix | (0x600 + query * 16);
        const uint32_t output = prefix | (0x700 + query * 8);
        actual.cpuRegs.GPR.r[4].UL[0] = expected.cpuRegs.GPR.r[4].UL[0] = axis;
        actual.cpuRegs.GPR.r[5].UL[0] = expected.cpuRegs.GPR.r[5].UL[0] = output;
        invoke(7, query + 1, Math::kSupportRangeAddress);
        ++expected_entries[7];
        if (!Equal(actual, expected, memory, expected_memory, math, 7, query + 2, originals, trial,
            selected, expected_entries)) return false;
    }
    return true;
}
}  // namespace

int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryCollisionOracle",
        Oracle::SourceDigest(), Oracle::Memory{}.bytes.size(), 8192);
    const int saved = std::fegetround();
    if (saved == -1 || std::fesetround(FE_TOWARDZERO) != 0) return 1;
    uint32_t seed = 0x434f4c4c;
    unsigned comparisons = 0;
    std::printf("MathLibraryCollisionOracle NEW source sha256=%s\n", Oracle::SourceDigest());
    for (unsigned operation = 0; operation < Oracle::kEntryCount; ++operation) {
        for (unsigned trial = 0; trial < Oracle::kCasesPerEntry; ++trial) {
            const bool sequence = operation == 0 || operation == 6;
            uint32_t control_seed = seed;
            if (!Compare(operation, trial, seed, sequence, true, artifacts) ||
                !Compare(operation, trial, control_seed, sequence, false, artifacts) || seed != control_seed) {
                (void)std::fesetround(saved);
                return 1;
            }
            comparisons += 2 * (operation == 0 ? 2 : operation == 6 ? 4 : 1);
        }
        std::printf("%s: %u installed entry cases per selection; continuations=0 unsupported=0\n",
            Oracle::Entries()[operation].name, Oracle::kCasesPerEntry);
    }
    if (std::fesetround(saved) != 0) return 1;
    std::printf("MathLibraryCollisionOracle PASS comparisons=%u selected_replacements=48000 "
                "selected_generated_bodies=0 control_generated_bodies=48000 differences=0\n", comparisons);
    if (comparisons != 96000) return 1;
    artifacts.Finish();
    return 0;
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryCollisionOracle: %s\n", error.what());
        return 1;
    }
}
