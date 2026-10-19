#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_animation_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle_artifacts.h"

#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/math/animation_math.h"
#include "games/slus_20909_crash_twinsanity/hle/math/trig_math.h"

uint8_t* MathLibraryAnimationOracle::Memory::Translate(uint32_t address, size_t length) {
    address = (address & 0x70000000u) == 0x70000000u
        ? 4096 + (address & 0x3fffu) : address & 0x1fffffffu;
    if (address > bytes.size() || length > bytes.size() - address) return nullptr;
    return bytes.data() + address;
}

uint32_t MathLibraryAnimationOracle::Memory::Read32(uint32_t address) {
    uint32_t value;
    auto* data = Translate(address, 4);
    if (!data) std::abort();
    std::memcpy(&value, data, 4);
    return value;
}

void MathLibraryAnimationOracle::Memory::Write32(uint32_t address, uint32_t value) {
    auto* data = Translate(address, 4);
    if (!data) std::abort();
    std::memcpy(data, &value, 4);
}

void MathLibraryAnimationOracle::Memory::Write16(uint32_t address, uint16_t value) {
    auto* data = Translate(address, 2);
    if (!data) std::abort();
    std::memcpy(data, &value, 2);
}

namespace {
using Oracle = MathLibraryAnimationOracle;
using Math = title::AnimationMath;
unsigned checks = 0, failures = 0;

void Check(bool pass, const char* message) {
    ++checks;
    if (pass) return;
    ++failures;
    std::fprintf(stderr, "Animation pin FAIL: %s\n", message);
}

uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

void ValuePins() {
    using title::TrigMath;
    // Historical signed-immediate bug: equal keys must stay equal, including
    // zero and negative keys. The previous defect added a full revolution.
    for (int sample = -32768; sample <= 32767; ++sample) {
        const auto pair = Math::DecodeAngles(static_cast<int16_t>(sample), static_cast<int16_t>(sample));
        Check(pair.current == pair.next && pair.current == TrigMath::AngleRadians(sample * 16),
            "equal keys never gain a revolution");
    }
    constexpr int32_t pairs[][3] = {
        {2048, 0, 32768}, {2049, 0, 32784 - 65536},
        {-2048, 0, -32768}, {-2049, 0, -32784 + 65536},
        {32767, -32768, 32767 * 16 - 65536},
        {-32768, 32767, -32768 * 16 + 65536},
        {100, 99, 1600}, {-100, -99, -1600},
    };
    for (const auto& input : pairs) {
        const auto pair = Math::DecodeAngles(static_cast<int16_t>(input[0]), static_cast<int16_t>(input[1]));
        Check(pair.current == TrigMath::AngleRadians(input[2]) &&
            pair.next == TrigMath::AngleRadians(input[1] * 16), "strict ties and exactly one wrap");
        const auto fixed = Math::DecodeAngles(static_cast<int16_t>(input[0]), static_cast<int16_t>(input[1]), true);
        Check(fixed.current == fixed.next && fixed.current == TrigMath::AngleRadians(input[0] * 16),
            "static channel ignores next and does not wrap");
    }
}

void InstallerPins() {
    Oracle::Memory memory;
    CpuContext context{};
    unsigned originals = 0;
    uint32_t received = 0;
    Math::Body callback;
    const auto capture = [&](uint32_t) -> Math::Body {
        return [&](CpuContext&, uint32_t entry) { ++originals; received = entry; };
    };
    {
        Math math(memory);
        unsigned attempts = 0;
        Check(!math.Install(capture, [&](uint32_t, Math::Body body) {
            if (attempts++ == 0) callback = std::move(body);
            callback(context, 0);
            return false;
        }), "failed installation");
        Check(math.Counts().angle_pairs == 0, "failed installation inactive");
    }
    const unsigned before = originals;
    callback(context, 0);
    Check(originals == before + 1, "inactive callback safe after destruction");
    {
        Math math(memory);
        Check(math.Install(capture, [&](uint32_t, Math::Body body) {
            callback = std::move(body);
            return true;
        }), "install decoder");
        callback(context, Math::kReadJointAngleAddress + 4);
        Check(received == Math::kReadJointAngleAddress + 4 &&
            math.Counts().generated_continuations == 1, "interior preserved");
        context.cpuRegs.GPR.r[0].UD[0] = 1;
        callback(context, 0);
        Check(math.Counts().unsupported_inputs == 1 && math.Counts().angle_pairs == 0, "noncanonical input delegates");
        context.cpuRegs.GPR.r[0].UD[0] = 0;
        context.cpuRegs.GPR.r[4].UL[0] = 0x10000000;
        callback(context, 0);
        Check(math.Counts().unsupported_inputs == 2, "unbacked reader delegates before read");
    }
    callback = {};
    for (const bool selected : {false, true}) {
        // Count actual entry even when the original subsequently throws.
        Math math(memory);
        Check(math.Install([&](uint32_t) -> Math::Body {
            return [&](CpuContext&, uint32_t) {
                Check(math.Counts().generated_body_calls == 1, "counter precedes original entry");
                throw std::runtime_error("original entered");
            };
        }, [&](uint32_t, Math::Body body) { callback = std::move(body); return true; }, selected), "selection install");
        bool caught = false;
        context.cpuRegs.GPR.r[0].UD[0] = 1;
        try { callback(context, 0); }
        catch (const std::runtime_error&) { caught = true; }
        Check(caught && math.Counts().generated_body_calls == 1, "throwing original counted");
        Check(math.Counts().unsupported_inputs == (selected ? 1 : 0), "control bypasses replacement guards");
        Check(math.Census().find("generated_body_angle_pairs=1") != std::string::npos, "actual census field");
    }
    callback = {};
    for (const bool selected : {false, true}) {
        for (unsigned behavior = 0; behavior < 3; ++behavior) {
            unsigned requests = 0;
            {
                Math math(memory);
                bool caught = false, result = false;
                try {
                    result = math.Install(capture, [&](uint32_t, Math::Body body) {
                        const unsigned request = requests++;
                        if (request != 0 && behavior != 0) {
                            if (behavior == 2) throw std::runtime_error("restore rejected");
                            return false;
                        }
                        callback = std::move(body);
                        if (request == 0 && behavior == 2) throw std::runtime_error("install rejected");
                        return request != 0;
                    }, selected);
                } catch (const std::runtime_error&) { caught = true; }
                Check(!result && caught == (behavior == 2), "failed selected/control status");
                Check(requests == 2 && math.Counts().angle_pairs == 0, "atomic decoder rollback");
            }
            const unsigned entered = originals;
            callback(context, 0);
            Check(originals == entered + 1, "rollback counted original safe after owner destruction");
        }
    }
}

void Seed(Oracle::Memory& memory, CpuContext& context, uint32_t& seed, unsigned trial, bool sequence) {
    for (auto& byte : memory.bytes) byte = static_cast<uint8_t>(Next(seed));
    for (size_t i = 0; i < sizeof(context); ++i)
        reinterpret_cast<uint8_t*>(&context)[i] = static_cast<uint8_t>(Next(seed));
    auto& r = context.cpuRegs.GPR.r;
    r[0].UD[0] = 0;
    constexpr uint32_t mirrors[] = {0, 0x80000000u, 0xa0000000u, 0x70000000u};
    const uint32_t prefix = mirrors[(trial / 16) % 4];
    const auto address = [&](uint32_t offset) { return prefix | (offset + trial % 16); };
    const uint32_t reader = address(0x100), stack = address(0x700);
    r[4].UL[0] = reader;
    r[5].UL[0] = !sequence && trial % 3 == 0 ? reader + (trial / 3 % 4) * 4 : address(0x500);
    r[6].UL[0] = !sequence && trial % 3 == 1 ? r[5].UL[0] : address(0x510);
    r[29].UL[0] = stack + 48;
    const uint16_t statics = static_cast<uint16_t>((Next(seed) & ~7u) | (trial % 8));
    memory.Write16(reader, statics);
    memory.Write32(reader + 4, address(0x200));
    memory.Write32(reader + 8, address(0x300));
    memory.Write32(reader + 12, !sequence && trial % 5 == 0 ? stack : address(0x400));
    constexpr int16_t keys[] = {0, -1, 1, -2049, -2048, 2048, 2049, -32768, 32767};
    for (uint32_t key = 0; key < 3; ++key) {
        const int16_t a = trial % 2 == 0 ? keys[(trial / 2 + key) % 9] : static_cast<int16_t>(Next(seed));
        const int16_t b = trial % 2 == 0 ? keys[(trial / 18 + key) % 9] : static_cast<int16_t>(Next(seed));
        memory.Write16(address(0x200) + key * 2, static_cast<uint16_t>(a));
        memory.Write16(address(0x300) + key * 2, static_cast<uint16_t>(a));
        memory.Write16(address(0x400) + key * 2, static_cast<uint16_t>(b));
    }
}

bool Compare(unsigned trial, uint32_t& seed, int mode, bool sequence, bool selected,
             MathLibraryOracleArtifacts& artifacts) {
    Oracle::Memory memory, expected_memory;
    CpuContext actual, expected;
    Seed(memory, actual, seed, trial, sequence);
    const uint32_t reader = actual.cpuRegs.GPR.r[4].UL[0];
    const uint32_t output_now = actual.cpuRegs.GPR.r[5].UL[0], output_next = actual.cpuRegs.GPR.r[6].UL[0];
    std::memcpy(&expected, &actual, sizeof(actual));
    expected_memory.bytes = memory.bytes;
    Math math(memory);
    unsigned originals = 0;
    Math::Body callback;
    if (!math.Install([&](uint32_t) -> Math::Body {
        return [&](CpuContext& context, uint32_t) { ++originals; Oracle::Invoke(context, memory); };
    }, [&](uint32_t, Math::Body body) { callback = std::move(body); return true; }, selected)) return false;
    for (unsigned axis = 0; axis < (sequence ? 3u : 1u); ++axis) {
        actual.cpuRegs.GPR.r[4].UL[0] = expected.cpuRegs.GPR.r[4].UL[0] = reader;
        actual.cpuRegs.GPR.r[5].UL[0] = expected.cpuRegs.GPR.r[5].UL[0] = output_now + axis * 4;
        actual.cpuRegs.GPR.r[6].UL[0] = expected.cpuRegs.GPR.r[6].UL[0] = output_next + axis * 4;
        const uint32_t dispatch = trial % 2 ? Math::kReadJointAngleAddress : 0;
        artifacts.Begin({Math::kReadJointAngleAddress, dispatch, static_cast<uint32_t>(sequence),
            trial, axis, selected}, expected, expected_memory.bytes.data());
        artifacts.RunModel([&] { Oracle::Invoke(expected, expected_memory); });
        artifacts.ModelOutput(expected, expected_memory.bytes.data());
        artifacts.CheckActualInput(actual, memory.bytes.data());
        callback(actual, dispatch);
        if (artifacts.Enabled()) artifacts.ActualOutput(actual, memory.bytes.data(), math.Census());
        const auto counts = math.Counts();
        if (memory.bytes == expected_memory.bytes && std::memcmp(&actual, &expected, sizeof(actual)) == 0 &&
            std::fegetround() == mode && counts.angle_pairs == (selected ? axis + 1 : 0) &&
            originals == (selected ? 0 : axis + 1) && counts.generated_body_calls == originals &&
            counts.generated_continuations == 0 && counts.unsupported_inputs == 0) continue;
        std::fprintf(stderr, "Animation oracle FAIL trial=%u axis=%u mode=%d originals=%u unsupported=%llu\n",
            trial, axis, mode, originals, static_cast<unsigned long long>(counts.unsupported_inputs));
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
    return true;
}
}  // namespace

int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryAnimationOracle",
        Oracle::SourceDigest(), Oracle::Memory{}.bytes.size(), 4096);
    const int saved = std::fegetround();
    if (saved == -1) return 1;
    uint32_t seed = 0x414e494d;
    unsigned comparisons = 0;
    std::printf("MathLibraryAnimationOracle NEW source sha256=%s\n", Oracle::SourceDigest());
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        if (std::fesetround(mode) != 0) return 1;
        ValuePins();
        for (unsigned trial = 0; trial < 4000; ++trial) {
            const bool sequence = trial >= 2000;
            uint32_t control_seed = seed;
            if (!Compare(trial, seed, mode, sequence, true, artifacts) ||
                !Compare(trial, control_seed, mode, sequence, false, artifacts) || seed != control_seed) {
                (void)std::fesetround(saved);
                return 1;
            }
            comparisons += sequence ? 6 : 2;
        }
    }
    InstallerPins();
    if (std::fesetround(saved) != 0) return 1;
    std::printf("MathLibraryAnimationOracle independent pins=%u failures=%u comparisons=%u "
                "selected_replacements=32000 selected_generated_bodies=0 control_generated_bodies=32000 "
                "unsupported=0 differences=0\n", checks, failures, comparisons);
    if (failures != 0 || comparisons != 64000) return 1;
    artifacts.Finish();
    return 0;
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryAnimationOracle: %s\n", error.what());
        return 1;
    }
}
