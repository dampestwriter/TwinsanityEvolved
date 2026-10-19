#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_clipping_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_dispatch_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle_artifacts.h"
#include "games/slus_20909_crash_twinsanity/hle/math/clipping_math.h"

#include <cfenv>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

uint8_t* MathLibraryClippingOracle::Memory::Translate(uint32_t address, size_t length) {
    address &= 0x1fffffffu;
    if (address > bytes.size() || length > bytes.size() - address) return nullptr;
    return bytes.data() + address;
}
uint32_t MathLibraryClippingOracle::Memory::Read32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, Translate(address, 4), 4);
    return value;
}
void MathLibraryClippingOracle::Memory::Write32(uint32_t address, uint32_t value) {
    std::memcpy(Translate(address, 4), &value, 4);
}

namespace {
using Oracle = MathLibraryClippingOracle;
using Math = title::ClippingMath;
constexpr std::array<uint32_t, 3> sources{0x2014d8, 0x20157c, Math::kCorners};
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
void Fill(uint8_t* bytes, size_t size, uint32_t& seed, unsigned trial) {
    for (size_t offset = 0; offset < size; offset += 4) {
        uint32_t word = Next(seed);
        if (trial % 3 == 0) word = Bits(static_cast<float>(static_cast<int32_t>(word)) / 65536.0f);
        std::memcpy(bytes + offset, &word, 4);
    }
}
void Fixture(CpuContext& context, Oracle::Memory& memory, uint32_t& seed, unsigned trial, size_t op) {
    Fill(reinterpret_cast<uint8_t*>(&context), sizeof(context), seed, trial);
    Fill(memory.bytes.data(), memory.bytes.size(), seed, trial);
    const uint32_t segment = std::array<uint32_t, 3>{0, 0x80000000, 0xa0000000}[trial % 3];
    const uint32_t bias = trial % 16;
    const uint32_t view = segment | (0x100u + bias);
    const uint32_t box = segment | (0x280u + bias);
    const uint32_t model = trial % 7 == 0 ? view : segment | (0x400u + bias);
    const uint32_t output = std::array<uint32_t, 6>{
        segment | (0x600u + bias), view, view + 64, box, model, model + 48}[trial % 6];
    auto& regs = context.cpuRegs.GPR.r;
    regs[4].UL[0] = view; regs[5].UL[0] = box;
    regs[6].UL[0] = output; regs[7].UL[0] = model;
    if (op == 2 && trial % 7 == 0)
        regs[4].UL[0] = regs[5].UL[0] = regs[7].UL[0] = 0xfffffff0;
    if (trial == 5) {
        const uint32_t raw[]{0x807f, 0x8000807f, 0x7fa01234, 0xff801234};
        for (uint32_t address : {view, box, model})
            std::memcpy(memory.Translate(address & ~15u, sizeof(raw)), raw, sizeof(raw));
        std::memcpy(context.vuRegs.VF[9].I, raw, sizeof(raw));
        context.vuRegs.clip_flag = 0xa53fe17c;
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
    const uint32_t source = sources[op], function = op == 0 ? sources[0] : sources[1];
    const uint32_t dispatch = op == 2 || trial % 2 ? source : 0;
    const int mode = std::fegetround();
    Oracle::Memory expected_memory, actual_memory;
    CpuContext expected{}, actual{};
    Fixture(expected, expected_memory, seed, trial, op);
    actual_memory.bytes = expected_memory.bytes;
    std::memcpy(&actual, &expected, sizeof(actual));
    artifacts.Begin({source, dispatch, static_cast<uint32_t>(op), trial, 0, selected},
        expected, expected_memory.bytes.data());
    artifacts.RunModel([&] { Oracle::Invoke(function, expected, expected_memory, dispatch); });
    artifacts.ModelOutput(expected, expected_memory.bytes.data());
    Math math(actual_memory);
    std::map<uint32_t, Math::Body> installed;
    Check(math.Install([&](uint32_t address) -> Math::Body {
        return [&, address](CpuContext& context, uint32_t entered) {
            Oracle::Invoke(address, context, actual_memory, entered, &installed.at(sources[1]));
        };
    }, [&](uint32_t address, Math::Body body) {
        return installed.emplace(address, std::move(body)).second;
    }, selected), "clipping group installs");
    artifacts.CheckActualInput(actual, actual_memory.bytes.data());
    const auto before = Oracle::Counts();
    installed.at(function)(actual, dispatch);
    const auto after = Oracle::Counts();
    if (artifacts.Enabled()) artifacts.ActualOutput(actual, actual_memory.bytes.data(), math.Census());
    const bool equal = expected_memory.bytes == actual_memory.bytes &&
        std::memcmp(&actual, &expected, sizeof(actual)) == 0;
    Check(equal, "whole clipping context/memory differs from actual NEW");
    if (!equal) Difference(actual, expected, actual_memory, expected_memory, source, trial, mode);
    const auto counts = math.Counts();
    for (size_t index = 0; index < Math::Entries().size(); ++index) {
        const uint64_t actual_original = !selected && (index == 1 || op == 0) ? 1 : 0;
        Check(after[index] - before[index] == actual_original &&
            counts.generated_body_calls[index] == actual_original,
            "actual generated body and source tail execution counted at execution");
    }
    Check(counts.box_at == (selected && op == 0 ? 1u : 0u) &&
        counts.box == (selected && op == 1 ? 1u : 0u) && counts.corners == (selected ? 1u : 0u),
        "selected source operations count actual complete corner batches");
    Check(counts.unsupported_entries == 0 && counts.unsupported_inputs == 0,
        "source entry closure needs no unsupported paths");
    Check(std::fegetround() == mode, "clipping preserves caller rounding");
    return equal;
}
void ValuePins() {
    const Math::Matrix identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    const Math::Box box{{-2, -2, 0, FromBits(0x807f)}, {2, 2, 0, FromBits(0x8000807f)}};
    const auto corners = Math::BoxCorners(box);
    Check(corners[2][0] == 2 && corners[2][1] == -2 && corners[3][1] == 2 &&
        corners[5][0] == -2 && corners[6][1] == -2 && corners[7][2] == 0,
        "source corner order mixes one axis at a time");
    Check(Bits(corners[2][3]) == 0x807f && Bits(corners[7][3]) == 0x8000807f,
        "R109b source copies preserve both raw w payloads");
    const auto guarded = Math::ClipBox(box, identity, {0.25f, 0.25f, 1, 1});
    Check(guarded.any == 0x3c0 && guarded.common == 0 && guarded.clipped[0][3] == 1,
        "tight XY planes and guard planes stay distinct; source corner w is ignored");
    const Math::Box outside{{3, 0, 0, 18}, {4, 0, 0, 99}};
    const auto rejected = Math::ClipBox(outside, identity, {1, 1, 1, 1});
    Check(rejected.any == 0x41 && rejected.common == 0x41,
        "common tight +X plane rejects every corner");
    auto negative_w = identity;
    negative_w[3][3] = -1;
    const auto negative = Math::ClipBox(outside, negative_w, {1, 1, 1, 1});
    Check(negative.any == rejected.any && negative.common == rejected.common,
        "R33/R44 negative w uses its absolute magnitude");
    const auto doubled = Math::ClipBox(box, identity, {1, 1, 1, 1}, nullptr, 2);
    Check(doubled.any == 0 && doubled.common == 0 && doubled.clipped[0][3] == 2,
        "live VF0.w affects translation; points exactly on the planes are inside");
    Oracle::Memory memory;
    Math math(memory);
    CpuContext context{};
    context.cpuRegs.GPR.r[6].UL[0] = 0x203;
    context.vuRegs.VF[0].F[3] = 1;
    context.vuRegs.clip_flag = 0xfedcba98;
    for (size_t i = 0; i < 8; ++i) std::memcpy(context.vuRegs.VF[1 + i].F, corners[i].data(), 16);
    for (size_t i = 0; i < 4; ++i) std::memcpy(context.vuRegs.VF[10 + i].F, identity[i].data(), 16);
    const Math::Vector guard{0.25f, 0.25f, 1, 1};
    std::memcpy(context.vuRegs.VF[9].F, guard.data(), 16);
    Check(math.Invoke(Math::kCorners, context) &&
        (memory.Read32(0x203) & 0xfff) == guarded.any &&
        (memory.Read32(0x207) & 0xfff) == guarded.common,
        "prior temporary flags cannot contaminate source-visible plane masks");
    for (uint32_t entry : sources) {
        context.cpuRegs.GPR.r[6].UL[0] = 4093;
        const auto before = context;
        const auto bytes = memory.bytes;
        Check(!math.Invoke(entry, context) && std::memcmp(&context, &before, sizeof(context)) == 0 &&
            bytes == memory.bytes, "invalid flags span refuses before any mutation");
    }
}
void ContinuationPins() {
    Oracle::Memory memory;
    for (bool selected : {false, true}) {
        MathLibraryDispatchFixture::Reset();
        for (const auto& entry : Math::Entries())
            MathLibraryDispatchFixture::Bind(entry.address, [&, address = entry.address]
                (CpuContext& context, uint32_t dispatch) {
                    const Math::Body corners = [](CpuContext& nested, uint32_t interior) {
                        if (!DispatchTable::InvokeAt(0x20157c, interior, nested))
                            throw std::runtime_error("missing ClipCorners");
                    };
                    Oracle::Invoke(address, context, memory, dispatch, &corners);
                });
        Math math(memory);
        Check(math.Install(DispatchTable::Capture, DispatchTable::Install, selected), "real clipping dispatch installs");
        Check(!math.Install(DispatchTable::Capture, DispatchTable::Install, selected), "duplicate install refuses");
        for (uint32_t entry : sources) {
            const uint32_t function = entry == sources[0] ? sources[0] : sources[1];
            CpuContext actual{}, expected{};
            Oracle::Memory expected_memory;
            uint32_t seed = 0x12345678;
            Fixture(actual, memory, seed, 5, entry == Math::kCorners ? 2 : 0);
            expected = actual; expected_memory.bytes = memory.bytes;
            Oracle::Invoke(function, expected, expected_memory, entry);
            Check(DispatchTable::CanResumeAt(function, entry) == !selected &&
                !DispatchTable::CanResumeAt(function, entry + 1),
                "control retains exact continuation metadata; selection invents no checkpoint proof");
            // Ordinary generated calls dispatch the installed callable. InvokeAt
            // is the stronger checkpoint API and correctly refuses opaque HLE.
            DispatchTable::Capture(function)(actual, entry);
            Check(std::memcmp(&actual, &expected, sizeof(actual)) == 0 && memory.bytes == expected_memory.bytes,
                "real dispatch supports all three source entries including the interior tail");
        }
        const auto counts = math.Counts();
        Check(counts.generated_body_calls[0] == (selected ? 0u : 1u) &&
            counts.generated_body_calls[1] == (selected ? 0u : 3u) &&
            counts.corners == (selected ? 3u : 0u),
            "real control tail counts actual second-body executions");
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
            "clipping captures both originals before publication");
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
                            "partially published clipping group remains original");
                        if (attempt == failure && throwing) throw std::runtime_error("publish failed");
                        return attempt != failure;
                    }, true);
                } catch (const std::runtime_error& error) {
                    refused = std::string(error.what()) == "publish failed";
                }
                Check(refused && math.Counts().corners == 0, "failed or throwing batch owns no corners");
            }
            for (const auto& [address, body] : retained) {
                CpuContext context{};
                const auto before = originals;
                body(context, address);
                Check(originals == before + 1 && context.cpuRegs.pc == address,
                    "failed callbacks remain safe after quiescent owner destruction");
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
        }, true), "clipping lifetime fixture installs");
        for (uint32_t entry : {0x2015d4u, Math::kCorners}) {
            CpuContext invalid{};
            invalid.cpuRegs.GPR.r[6].UL[0] = 4093;
            bool refused = false;
            try { retained.at(0x20157c)(invalid, entry); }
            catch (const std::runtime_error&) { refused = true; }
            Check(refused && originals == 0, "unsupported selected request never silently executes original");
        }
        Check(math.Counts().unsupported_entries == 1 && math.Counts().unsupported_inputs == 1,
            "unsupported source entries and invalid inputs have separate counters");
    }
    CpuContext context{};
    retained.at(0x20157c)(context, Math::kCorners);
    Check(originals == 1, "retained callback safely forwards after quiescent owner teardown");
}
int Run(int argc, char** argv) {
    MathLibraryOracleArtifacts artifacts(argc, argv, "MathLibraryClippingOracle", Oracle::SourceDigest(),
        Oracle::Memory{}.bytes.size(), 0);
    const int previous = std::fegetround();
    ValuePins();
    ContinuationPins();
    InstallationPins();
    uint64_t cases = 0;
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        Check(std::fesetround(mode) == 0, "set oracle rounding");
        uint32_t seed = 0x434c4950;
        for (size_t op = 0; op < sources.size(); ++op) {
            for (unsigned trial = 0; trial < 4000; ++trial) {
                auto control_seed = seed;
                if (!Compare(op, trial, seed, true, artifacts) ||
                    !Compare(op, trial, control_seed, false, artifacts)) return 1;
                Check(seed == control_seed, "selected and actual control consume identical fixtures");
                cases += 2;
            }
        }
    }
    std::fesetround(previous);
    const auto counts = Oracle::Counts();
    std::printf("MathLibraryClippingOracle checks=%u failures=%u cases=%llu actual_new=%llu/%llu\n",
        checks, failures, static_cast<unsigned long long>(cases),
        static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(counts[1]));
    if (!failures) artifacts.Finish();
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    try { return Run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "MathLibraryClippingOracle: %s\n", error.what());
        return 1;
    }
}
