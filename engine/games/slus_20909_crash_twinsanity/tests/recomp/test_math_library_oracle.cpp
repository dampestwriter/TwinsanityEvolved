#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle_artifacts.h"

#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

uint8_t* MathLibraryOracle::Memory::Translate(uint32_t address, size_t length) {
    // Distinct fixture storage for scratchpad and RAM; the two KSEG aliases
    // point at the same ordinary data bytes as their low-address counterparts.
    address = (address & 0x70000000u) == 0x70000000u
        ? 2048 + (address & 0x3fffu) : address & 0x1fffffffu;
    if (address > bytes.size() || length > bytes.size() - address) {
        std::fprintf(stderr, "MathLibraryGeneratedOracle: invalid fixture memory %#x/%zu\n", address, length);
        std::abort();
    }
    return bytes.data() + address;
}

uint32_t MathLibraryOracle::Memory::Read32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, Translate(address, sizeof(value)), sizeof(value));
    return value;
}

void MathLibraryOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, sizeof(value)), &value, sizeof(value));
}

namespace {

using Oracle = MathLibraryOracle;
using Math = title::MathLibrary;

uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

void SeedContext(CpuContext& context, uint32_t& seed, const Oracle::Entry& entry, unsigned trial) {
    for (size_t i = 0; i < sizeof(context); ++i)
        reinterpret_cast<uint8_t*>(&context)[i] = static_cast<uint8_t>(Next(seed));
    if ((trial & 1) == 0) {
        const std::array<float, 4> constant{0, 0, 0, 1};
        std::memcpy(context.vuRegs.VF[0].F, constant.data(), sizeof(constant));
    }
    constexpr uint32_t mirrors[] = {0, 0x80000000u, 0xa0000000u, 0x70000000u};
    constexpr uint32_t destinations[] = {0x100, 0x110, 0x120, 0x130, 0x200, 0x210, 0x300, 0x500};
    const uint32_t prefix = mirrors[trial % 4];
    const uint32_t first = prefix | (0x100u + trial % 16);
    const uint32_t second = prefix | ((trial % 5 == 0 ? 0x110u : 0x200u) + trial % 16);
    const uint32_t output = prefix | destinations[trial % 8] | (trial % 16);
    auto& registers = context.cpuRegs.GPR.r;
    registers[4].UL[0] = first;
    registers[5].UL[0] = second;
    registers[6].UL[0] = output;
    switch (entry.argument_layout) {
        case 0: break;  // Two inputs and one output.
        case 1: registers[4].UL[0] = output; break;
        case 2:
            registers[4].UL[0] = output;
            registers[5].UL[0] = first;
            registers[6].UL[0] = second;
            break;
        case 3:
            registers[4].UL[0] = output;
            registers[5].UL[0] = first;
            break;
        case 4:
            registers[4].UL[0] = first;
            registers[5].UL[0] = output;
            break;
        case 5: registers[4].UL[0] = first; break;
        case 6:
            registers[4].UL[0] = output;
            registers[5].UL[0] = first;
            registers[6].UL[0] = second;
            registers[7].UL[0] = prefix | 0x400u;
            registers[29].UL[0] = prefix | 0x510u | (trial % 16);
            break;
        default: std::abort();
    }
}

uint64_t Replacements(const Math::Counters& count) {
    return count.transform_point + count.rotate_vector + count.transform_vector +
        count.multiply_matrices + count.identity_matrix + count.multiply_rotations +
        count.rotation_matrix + count.transpose + count.transpose_in_place +
        count.transpose_rotation + count.invert_rigid + count.invert_rigid_in_place + count.triangle_plane;
}

void ReportDifferences(const CpuContext& actual, const CpuContext& expected,
                       const Oracle::Memory& actual_memory, const Oracle::Memory& expected_memory) {
    for (size_t offset = 0; offset < sizeof(expected); offset += 4) {
        uint32_t a, b;
        std::memcpy(&a, reinterpret_cast<const uint8_t*>(&actual) + offset, sizeof(a));
        std::memcpy(&b, reinterpret_cast<const uint8_t*>(&expected) + offset, sizeof(b));
        if (a != b) std::fprintf(stderr, " context+%zu actual=%08x expected=%08x\n", offset, a, b);
    }
    for (size_t offset = 0; offset < actual_memory.bytes.size(); ++offset) {
        if (actual_memory.bytes[offset] == expected_memory.bytes[offset]) continue;
        std::fprintf(stderr, " first memory difference +%zu actual=%02x expected=%02x\n", offset,
            static_cast<unsigned>(actual_memory.bytes[offset]), static_cast<unsigned>(expected_memory.bytes[offset]));
        break;
    }
}

bool Compare(const Oracle::Entry& entry, unsigned trial, uint32_t& seed, bool selected,
             MathLibraryOracleArtifacts& artifacts) {
    Oracle::Memory expected_memory, actual_memory;
    for (auto& byte : expected_memory.bytes) byte = static_cast<uint8_t>(Next(seed));
    actual_memory.bytes = expected_memory.bytes;
    CpuContext expected, actual;
    SeedContext(expected, seed, entry, trial);
    std::memcpy(&actual, &expected, sizeof(actual));
    const uint32_t dispatch = trial % 2 ? entry.address : 0;
    artifacts.Begin({entry.address, dispatch, static_cast<uint32_t>(&entry - Oracle::Entries().data()),
        trial, 0, selected}, expected, expected_memory.bytes.data());
    artifacts.RunModel([&] { Oracle::Invoke(entry.address, expected, expected_memory); });
    artifacts.ModelOutput(expected, expected_memory.bytes.data());

    Math math(actual_memory);
    std::map<uint32_t, Math::Body> installed;
    unsigned original_calls = 0;
    const auto capture = [&](uint32_t address) -> Math::Body {
        for (const auto& original : Oracle::Entries()) {
            if (original.address != address) continue;
            return [&, address](CpuContext& context, uint32_t) {
                ++original_calls;
                Oracle::Invoke(address, context, actual_memory);
            };
        }
        return {};
    };
    const auto install = [&](uint32_t address, Math::Body body) {
        return installed.emplace(address, std::move(body)).second;
    };
    if (!math.Install(capture, install, selected) || installed.size() != Oracle::kEntryCount) {
        std::fprintf(stderr, "MathLibraryGeneratedOracle: incomplete installation\n");
        return false;
    }
    artifacts.CheckActualInput(actual, actual_memory.bytes.data());
    installed.at(entry.address)(actual, dispatch);
    if (artifacts.Enabled()) artifacts.ActualOutput(actual, actual_memory.bytes.data(), math.Census());
    const auto count = math.Counts();
    const unsigned expected_originals = !selected && entry.address != Math::kTransformPointAddress ? 1 : 0;
    uint64_t generated = 0;
    for (const auto value : count.generated_body_calls) generated += value;
    size_t slot = 0;
    while (Oracle::Entries()[slot].address != entry.address) ++slot;
    if (slot < 2) slot ^= 1;  // Census order starts point, rotate.
    const bool equal = expected_memory.bytes == actual_memory.bytes &&
        std::memcmp(&expected, &actual, sizeof(actual)) == 0 &&
        std::fegetround() == FE_TOWARDZERO && Replacements(count) == 1 - expected_originals &&
        original_calls == expected_originals && count.generated_continuations == 0 &&
        generated == expected_originals && count.generated_body_calls[slot] == expected_originals;
    if (!equal) {
        std::fprintf(stderr,
            "MathLibraryGeneratedOracle: FAIL %s entry=%#x trial=%u seed=%08x "
            "replacement=%llu generated=%u continuations=%llu\n", entry.name, entry.address, trial, seed,
            static_cast<unsigned long long>(Replacements(count)), original_calls,
            static_cast<unsigned long long>(count.generated_continuations));
        ReportDifferences(actual, expected, actual_memory, expected_memory);
    }
    return equal;
}

}  // namespace

int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryGeneratedOracle",
        Oracle::SourceDigest(), Oracle::Memory{}.bytes.size(), 2048, Math::kTransformPointAddress);
    const int saved_rounding = std::fegetround();
    if (saved_rounding == -1 || std::fesetround(FE_TOWARDZERO) != 0) return 1;
    // Same seed, byte stream, aliases, call order and 4,000 cases/entry as the
    // accepted 52k standalone audit. No ELF, capture, savestate or OLD tree.
    uint32_t seed = 0x4d314142;
    unsigned total = 0;
    std::printf("MathLibraryGeneratedOracle: NEW source sha256=%s\n", Oracle::SourceDigest());
    for (const auto& entry : Oracle::Entries()) {
        for (unsigned trial = 0; trial < Oracle::kCasesPerEntry; ++trial) {
            uint32_t control_seed = seed;
            if (!Compare(entry, trial, seed, true, artifacts) ||
                !Compare(entry, trial, control_seed, false, artifacts) ||
                seed != control_seed) {
                (void)std::fesetround(saved_rounding);
                return 1;
            }
            total += 2;
        }
        std::printf("%s: selected=%u control=%u selected_generated_bodies=0 control_generated_bodies=%u differences=0\n",
                    entry.name, Oracle::kCasesPerEntry, Oracle::kCasesPerEntry,
                    entry.address == Math::kTransformPointAddress ? 0 : Oracle::kCasesPerEntry);
    }
    if (std::fesetround(saved_rounding) != 0) return 1;
    std::printf("MathLibraryGeneratedOracle: PASS cases=%u selected_generated_bodies=0 "
                "control_generated_bodies=48000 control_transform_point_replacements=4000 differences=0\n", total);
    if (total != 104000) return 1;
    artifacts.Finish();
    return 0;
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryGeneratedOracle: %s\n", error.what());
        return 1;
    }
}
