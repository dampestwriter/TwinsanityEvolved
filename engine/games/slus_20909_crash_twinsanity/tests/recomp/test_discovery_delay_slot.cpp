// RecompDiscoveryDelaySlotRule -- synthetic-fixture pin for the recompiler's discovery
// rule that fixed BUG-072: an address that is the delay slot of the preceding
// branch/jump is never a function start, and every address flow can enter from
// outside a function is a leader (a `switch (addr)` case) of the function that
// contains it.
//
// Fixture DS-BUG072 (hand-assembled R5900 words, no title addresses): the retail
// shape of 0x178470 -- a guard branch whose delay slot is the prologue
// `addiu sp,sp,-0x10`, executed on both paths, one epilogue per path. The function
// F is referenced only by a data pointer (weak seed), so the prologue word in the
// delay slot is the first weak seed discovery considers. Before the rule, F was
// split into two functions at the delay slot and the fall-through address had no
// case; with the rule it is ONE function whose leaders include both paths.
//
//   0x100000 entry:  addiu sp,-0x10; sw ra,0(sp); jal G; nop; lw ra,0(sp); addiu sp,0x10; jr ra; nop
//   0x100020 F:      lhu v0,0x1c(a3); andi v0,v0,1; bne v0,zero,T; addiu sp,sp,-0x10   (slot)
//   0x100030 FT:     addiu v0,zero,1; addiu sp,sp,0x10; jr ra; nop
//   0x100040 T:      addiu v0,zero,2; addiu sp,sp,0x10; jr ra; nop
//   0x100050 G:      jr ra; nop
//   rodata 0x200000: pointer -> F
#include <cstdint>
#include <cstdio>
#include <vector>

#include "recompiler_tool/discovery/functions.h"

namespace {
int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

// The fixture builder (the file's one primary type).
class TestDiscoveryDelaySlot {
public:
    static constexpr uint32_t kText = 0x00100000;
    static constexpr uint32_t kEntry = kText;
    static constexpr uint32_t kF = kText + 0x20;
    static constexpr uint32_t kSlot = kText + 0x2c;
    static constexpr uint32_t kFallThrough = kText + 0x30;
    static constexpr uint32_t kTaken = kText + 0x40;
    static constexpr uint32_t kG = kText + 0x50;
    static constexpr uint32_t kEnd = kText + 0x58;
    static constexpr uint32_t kRodata = 0x00200000;

    recomp_v2::DiscoveryInputs Build() const {
        std::vector<uint32_t> w = {
            // entry
            0x27bdfff0u,                       // addiu sp,sp,-0x10
            0xafbf0000u,                       // sw ra,0(sp)
            0x0c000000u | ((kG >> 2) & 0x03ffffffu),  // jal G
            0x00000000u,                       // nop
            0x8fbf0000u,                       // lw ra,0(sp)
            0x27bd0010u,                       // addiu sp,sp,0x10
            0x03e00008u,                       // jr ra
            0x00000000u,                       // nop
            // F
            0x94e2001cu,                       // lhu v0,0x1c(a3)
            0x30420001u,                       // andi v0,v0,1
            0x14400000u | (((kTaken - (kF + 0x8 + 4)) >> 2) & 0xffffu),  // bne v0,zero,T
            0x27bdfff0u,                       // addiu sp,sp,-0x10   (delay slot: the prologue)
            // FT
            0x24020001u,                       // addiu v0,zero,1
            0x27bd0010u,                       // addiu sp,sp,0x10
            0x03e00008u,                       // jr ra
            0x00000000u,                       // nop
            // T
            0x24020002u,                       // addiu v0,zero,2
            0x27bd0010u,                       // addiu sp,sp,0x10
            0x03e00008u,                       // jr ra
            0x00000000u,                       // nop
            // G
            0x03e00008u,                       // jr ra
            0x00000000u,                       // nop
        };
        recomp_v2::DiscoveryInputs in;
        in.text.name = ".text";
        in.text.base_va = kText;
        for (uint32_t word : w)
            for (int b = 0; b < 4; ++b) in.text.bytes.push_back(static_cast<uint8_t>(word >> (8 * b)));
        recomp_v2::SectionImage ro;
        ro.name = ".rodata";
        ro.base_va = kRodata;
        for (int b = 0; b < 4; ++b) ro.bytes.push_back(static_cast<uint8_t>(kF >> (8 * b)));
        in.data.push_back(ro);
        in.elf_entry = kEntry;
        return in;
    }
};
}  // namespace

int main() {
    TestDiscoveryDelaySlot fx;
    const recomp_v2::DiscoveryInputs in = fx.Build();
    CHECK(in.text.bytes.size() == TestDiscoveryDelaySlot::kEnd - TestDiscoveryDelaySlot::kText, "fixture image is 22 words");

    const recomp_v2::DiscoveryResult r = recomp_v2::discover_program(in);
    CHECK(r.engine_ran, "discovery engine ran");
    std::printf("functions:");
    for (const auto& kv : r.functions) std::printf(" 0x%08x[0x%08x,0x%08x)", kv.first, kv.second.entry, kv.second.end);
    std::printf("\n");

    CHECK(r.functions.size() == 3, "exactly three functions: entry, F, G");
    CHECK(r.functions.count(TestDiscoveryDelaySlot::kEntry) == 1, "entry is a function");
    CHECK(r.functions.count(TestDiscoveryDelaySlot::kG) == 1, "G (jal target) is a function");
    CHECK(r.functions.count(TestDiscoveryDelaySlot::kF) == 1, "F (data-pointer seed) is a function");
    CHECK(r.functions.count(TestDiscoveryDelaySlot::kSlot) == 0, "the delay-slot prologue word is NOT a function start (BUG-072)");
    CHECK(r.delay_slot_weak_rejected.count(TestDiscoveryDelaySlot::kSlot) == 1, "the rejected weak seed is reported");
    CHECK(r.delay_slot_strong_entries.empty(), "no strong entry sits in a delay slot");

    auto f = r.functions.find(TestDiscoveryDelaySlot::kF);
    if (f != r.functions.end()) {
        CHECK(f->second.end == TestDiscoveryDelaySlot::kG, "F's extent runs to G (both paths inside one function)");
        CHECK(f->second.leaders.count(TestDiscoveryDelaySlot::kFallThrough) == 1, "F has a leader (switch case) at the fall-through 0x100030");
        CHECK(f->second.leaders.count(TestDiscoveryDelaySlot::kTaken) == 1, "F has a leader (switch case) at the branch target 0x100040");
        CHECK(f->second.leaders.count(TestDiscoveryDelaySlot::kSlot) == 0, "the delay slot itself is not a leader (it executes with its branch)");
        CHECK(f->second.seeds.count(recomp_v2::SeedSource::kDataPointer) == 1, "F carries the data-pointer seed");
    }
    CHECK(r.unreached_gaps.empty(), "every word of the image is reached (no code lost by the rule)");

    std::printf("%s\n", g_fail ? "RecompDiscoveryDelaySlotRule: FAIL" : "RecompDiscoveryDelaySlotRule: PASS");
    return g_fail ? 1 : 0;
}
