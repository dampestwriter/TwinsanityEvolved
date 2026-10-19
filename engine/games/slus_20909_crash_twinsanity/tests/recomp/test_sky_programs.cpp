#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_programs.h"

#include <cstdio>
#include <cstring>
#include <vector>

// SkyPrograms: the sky material's VU1 program residency as writes. The retail
// arm is judged by SkyPassSelfOracle (model vs owned residency on the static
// sky assets); this pins the planner's rules and its refusals on a fixture.
namespace {
using P = title::SkyPrograms;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("SkyPrograms line%d: %s\n", __LINE__, #x); } } while (false)
struct Fixture {
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x400000);
    static constexpr uint32_t kMaterial = 0x13000, kShader = 0x14000, kChainNext = 0x21000, kLastTag = 0x20000;
    static constexpr uint32_t kCode = 0x2e0000, kProgram = 0x1b, kSize = 0x1b4;   // 436 instructions: 240 + 196
    template<class T> void Put(uint32_t at, const T& value) { std::memcpy(memory.data() + at, &value, sizeof(T)); }
    template<class T> T Get(uint32_t at) const { T value; std::memcpy(&value, memory.data() + at, sizeof(T)); return value; }
    Fixture() {
        Put<uint32_t>(kMaterial + P::kMaterialShaders, kShader);
        Put<uint32_t>(kMaterial + P::kMaterialShaderCount, 1);
        Put<uint64_t>(kMaterial + P::kMaterialKey, 0x10);
        Put<uint32_t>(kMaterial + P::kMaterialBucket, 0);
        Put<uint32_t>(kShader + P::kShaderVtable, P::kTypeAVtable);
        Put<uint32_t>(P::kTypeAProgram, kProgram);
        Put<uint32_t>(P::kRegion1, 1394); Put<uint32_t>(P::kRegion2, 740);
        Put<uint32_t>(P::kTime, 7); Put<uint32_t>(P::kNext, 999);
        const uint32_t bucket = P::kBuckets;
        Put<uint32_t>(bucket + P::kBucketLast, kLastTag);
        Put<uint8_t>(bucket + P::kBucketRegion, 0);
        Put<int32_t>(bucket + P::kBucketChain, 0);
        Put<uint64_t>(bucket + P::kBucketLastKey, 0);
        Put<uint32_t>(bucket + P::kBucketLastCall, 0x1234);
        Put<uint32_t>(P::kChains + P::kChainNext, kChainNext);
        const uint32_t entry = P::kPrograms + kProgram * P::kProgramStride;
        Put<uint32_t>(entry + P::kProgramCode, kCode);
        Put<uint32_t>(entry + P::kProgramSize, kSize);
        Put<uint32_t>(entry + P::kProgramLoaded, 0xffffffff);
    }
    title::MaterialState::Read Reader() {
        return [this](uint32_t at, void* out, size_t size) {
            if (at > memory.size() || size > memory.size() - at) return false;
            std::memcpy(out, memory.data() + at, size);
            return true;
        };
    }
    void Apply(const std::vector<P::Write>& writes) {
        for (const auto& write : writes) {
            CHECK(write.address < memory.size() && write.bytes.size() <= memory.size() - write.address);
            std::memcpy(memory.data() + write.address, write.bytes.data(), write.bytes.size());
        }
    }
};
void TestLoad() {
    Fixture f;
    P programs;
    CHECK(programs.Capture(f.Reader()));
    P::Plan plan;
    CHECK(programs.Step(f.Reader(), Fixture::kMaterial, plan) == P::Result::Ok);
    CHECK(plan.uploads == 1);
    f.Apply(plan.writes);
    // StartsPrograms: the bucket keyed, region 2's start taken, the time advanced.
    CHECK(f.Get<uint64_t>(P::kBuckets + P::kBucketLastKey) == 0x10);
    CHECK(f.Get<uint32_t>(P::kTime) == 8 && f.Get<uint32_t>(P::kStart) == 740 && f.Get<uint32_t>(P::kBucket) == 0);
    CHECK(f.Get<uint32_t>(P::kNext) == 740 + Fixture::kSize);
    CHECK(f.Get<uint32_t>(P::kBuckets + P::kBucketLastCall) == 0);
    // LoadProgramForMaterial: the entry's address, time and bucket slot.
    const uint32_t entry = P::kPrograms + Fixture::kProgram * P::kProgramStride;
    CHECK(f.Get<uint32_t>(entry + P::kProgramAddress) == 740 && f.Get<uint32_t>(entry + P::kProgramLoaded) == 8);
    CHECK(f.Get<uint16_t>(entry + P::kProgramBucketAddresses) == 740);
    // BeginPacket/WriteProgramUpload/EndPacket: the last tag points at the
    // packet; CNT (FLUSHE, STCYCL), two REF MPG chunks (240 + 196), a NEXT tag.
    CHECK(f.Get<uint32_t>(Fixture::kLastTag + 4) == Fixture::kChainNext);
    const uint32_t at = Fixture::kChainNext;
    CHECK(f.Get<uint32_t>(at) == P::kCntTag && f.Get<uint32_t>(at + 4) == 0 &&
          f.Get<uint32_t>(at + 8) == P::kVifFlushE && f.Get<uint32_t>(at + 12) == P::kVifCycle1);
    CHECK(f.Get<uint32_t>(at + 16) == (P::kRefTag | 120) && f.Get<uint32_t>(at + 20) == Fixture::kCode &&
          f.Get<uint32_t>(at + 24) == 0 && f.Get<uint32_t>(at + 28) == (P::kVifMpg | 240u << 16 | 740));
    CHECK(f.Get<uint32_t>(at + 32) == (P::kRefTag | 98) && f.Get<uint32_t>(at + 36) == Fixture::kCode + 240 * 8 &&
          f.Get<uint32_t>(at + 44) == (P::kVifMpg | 196u << 16 | (740 + 240)));
    CHECK(f.Get<uint32_t>(at + 48) == P::kNextTag && f.Get<uint32_t>(at + 52) == 0);
    CHECK(f.Get<uint32_t>(P::kBuckets + P::kBucketLast) == at + 48);
    CHECK(f.Get<uint32_t>(P::kChains + P::kChainNext) == at + 64);
    // A second material of the same key in the same pass loads nothing more:
    // the bucket word and the last call only.
    P::Plan again;
    CHECK(programs.Step(f.Reader(), Fixture::kMaterial, again) == P::Result::Ok);
    CHECK(again.uploads == 0 && again.writes.size() == 2);
    // A fresh pass whose bucket already carries the key loads nothing either;
    // keyed 1 (NoProgramsKey) never loads.
    P fresh;
    CHECK(fresh.Capture(f.Reader()));
    P::Plan none;
    CHECK(fresh.Step(f.Reader(), Fixture::kMaterial, none) == P::Result::Ok && none.uploads == 0);
    Fixture g;
    g.Put<uint64_t>(Fixture::kMaterial + P::kMaterialKey, P::kNoProgramsKey);
    P keyed; CHECK(keyed.Capture(g.Reader()));
    P::Plan nothing;
    CHECK(keyed.Step(g.Reader(), Fixture::kMaterial, nothing) == P::Result::Ok && nothing.uploads == 0);
    CHECK(nothing.writes.size() == 2 && g.Get<uint32_t>(P::kTime) == 7);
}
void TestRegionAndResidents() {
    // Region byte 1 takes region 1's start; a resident program (below 0xF) is
    // never loaded by a material; a program already loaded this time is skipped.
    Fixture f;
    f.Put<uint8_t>(P::kBuckets + P::kBucketRegion, 1);
    P programs; CHECK(programs.Capture(f.Reader()));
    P::Plan plan;
    CHECK(programs.Step(f.Reader(), Fixture::kMaterial, plan) == P::Result::Ok && plan.uploads == 1);
    f.Apply(plan.writes);
    CHECK(f.Get<uint32_t>(P::kStart) == 1394 && f.Get<uint32_t>(P::kNext) == 1394 + Fixture::kSize);
    Fixture g;
    g.Put<uint32_t>(P::kTypeAProgram, 0xa);
    P resident; CHECK(resident.Capture(g.Reader()));
    P::Plan none;
    CHECK(resident.Step(g.Reader(), Fixture::kMaterial, none) == P::Result::Ok && none.uploads == 0);
    g.Apply(none.writes);
    CHECK(g.Get<uint32_t>(P::kTime) == 8 && g.Get<uint32_t>(P::kNext) == 740);   // the region was still taken
}
void TestRefusals() {
    struct Case { const char* name; void (*mutate)(Fixture&); P::Result result; };
    const Case cases[] = {
        {"unknown shader vtable", [](Fixture& f) { f.Put<uint32_t>(Fixture::kShader + P::kShaderVtable, 0x1000); }, P::Result::UnsupportedState},
        {"program index out of table", [](Fixture& f) { f.Put<uint32_t>(P::kTypeAProgram, P::kProgramCount); }, P::Result::UnsupportedState},
        {"bucket out of range", [](Fixture& f) { f.Put<uint32_t>(Fixture::kMaterial + P::kMaterialBucket, 28); }, P::Result::UnsupportedState},
        {"shader count out of range", [](Fixture& f) { f.Put<uint32_t>(Fixture::kMaterial + P::kMaterialShaderCount, 5); }, P::Result::UnsupportedState},
        {"chain index out of range", [](Fixture& f) { f.Put<int32_t>(P::kBuckets + P::kBucketChain, 10); }, P::Result::UnsupportedState},
        {"unaligned last tag", [](Fixture& f) { f.Put<uint32_t>(P::kBuckets + P::kBucketLast, 0x20004); }, P::Result::UnsupportedState},
        {"unaligned chain cursor", [](Fixture& f) { f.Put<uint32_t>(P::kChains + P::kChainNext, 0x21008); }, P::Result::UnsupportedState},
        {"chain cursor past its buffer", [](Fixture& f) {
            f.Put<uint32_t>(P::kChains + P::kChainStart, 0x21000); f.Put<uint32_t>(P::kChains + P::kChainCapacity, 3); }, P::Result::UnsupportedState},
        {"odd program size", [](Fixture& f) { f.Put<uint32_t>(P::kPrograms + Fixture::kProgram * P::kProgramStride + P::kProgramSize, 7); }, P::Result::UnsupportedState},
        {"program past micro memory", [](Fixture& f) { f.Put<uint32_t>(P::kRegion2, 2000); }, P::Result::UnsupportedState},
        {"unaligned code", [](Fixture& f) { f.Put<uint32_t>(P::kPrograms + Fixture::kProgram * P::kProgramStride + P::kProgramCode, 0x2e0008); }, P::Result::UnsupportedState},
        {"shader pointer unreadable", [](Fixture& f) { f.Put<uint32_t>(Fixture::kMaterial + P::kMaterialShaders, 0x3ffffffc); }, P::Result::InvalidMemory},
    };
    for (const auto& item : cases) {
        Fixture f;
        item.mutate(f);
        P programs; CHECK(programs.Capture(f.Reader()));
        P::Plan plan;
        const auto result = programs.Step(f.Reader(), Fixture::kMaterial, plan);
        if (result != item.result) std::printf("SkyPrograms refusal %s: %d\n", item.name, int(result));
        CHECK(result == item.result);
    }
    // No capture: nothing is planned.
    P idle; P::Plan plan; Fixture f;
    CHECK(idle.Step(f.Reader(), Fixture::kMaterial, plan) == P::Result::UnsupportedState && plan.writes.empty());
    // A reader that cannot see the globals refuses the capture.
    P blind;
    CHECK(!blind.Capture([](uint32_t, void*, size_t) { return false; }));
}
}  // namespace

int main() {
    TestLoad();
    TestRegionAndResidents();
    TestRefusals();
    std::printf("SkyPrograms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
