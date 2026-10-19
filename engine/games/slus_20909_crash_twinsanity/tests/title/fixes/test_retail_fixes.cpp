// RetailFixes: the owned bodies over the title's constructors.
//   * nothing selected -> nothing captured or installed (the title as shipped);
//   * look_rest_seconds: the fake constructor runs (its stores land, v0 = a0),
//     then restSeconds (+0x60) is written 0; a mid-body continuation does not re-apply;
//   * skid_mark_init: lastPoint (+0x10) = (0,0,0,1), edgeColour 0x80FFFFFF, lowColour
//     0x00FFFFFF after the fake body; a0 is read at entry (the body may clobber it);
//   * a refused capture (unknown address) reports false; the census names each toggle.
#include <cstdio>
#include <map>
#include <string>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/fixes/retail_fixes.h"

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

using title::RetailFixes;

struct TestRetailFixes {
    std::map<uint32_t, uint32_t> ram;
    std::map<uint32_t, RetailFixes::Body> table;
    int captures = 0, installs = 0, body_calls = 0;
    TestRetailFixes() {
        table[RetailFixes::kLookControllerConstruct] = [this](CpuContext& ctx, uint32_t) {
            ++body_calls;
            const uint32_t look = ctx.cpuRegs.GPR.r[4].UL[0];
            ram[look + 0x0] = 0x2f3230;   // vtable
            ram[look + 0x40] = 0x3f000000;
            ctx.cpuRegs.GPR.r[2].UL[0] = look;   // returns its argument
            ctx.cpuRegs.GPR.r[4].UL[0] = 0;      // a0 clobbered, as a callee may
        };
        table[RetailFixes::kConstructSkidMarks] = [this](CpuContext& ctx, uint32_t) {
            ++body_calls;
            const uint32_t marks = ctx.cpuRegs.GPR.r[4].UL[0];
            ram[marks + 0x1c8] = 0;
            ctx.cpuRegs.GPR.r[4].UL[0] = 0xdeadbeef;
        };
    }
    RetailFixes::Capture capture() {
        return [this](uint32_t a) -> RetailFixes::Body { ++captures; auto it = table.find(a); return it == table.end() ? nullptr : it->second; };
    }
    RetailFixes::Installer installer() {
        return [this](uint32_t a, RetailFixes::Body b) { ++installs; table[a] = std::move(b); return true; };
    }
    RetailFixes::Write32 write32() { return [this](uint32_t a, uint32_t v) { ram[a] = v; }; }
};

int main() {
    {
        TestRetailFixes t;
        RetailFixes none(RetailFixes::Selection{}, t.write32());
        CHECK(none.Install(t.capture(), t.installer()) && t.captures == 0 && t.installs == 0, "nothing_selected_installs_nothing");
        CHECK(none.Census() == "look_rest_seconds=off,applied=0;skid_mark_init=off,applied=0;installed=0", "census_off");
    }
    {
        TestRetailFixes t;
        RetailFixes fx(RetailFixes::Selection{true, true}, t.write32());
        CHECK(fx.Install(t.capture(), t.installer()) && t.installs == 2, "both_installed");
        CpuContext ctx{};
        ctx.cpuRegs.GPR.r[4].UL[0] = 0x1000;
        t.ram[0x1000 + 0x60] = 0x7fc00000;   // the heap's leftover (a NaN)
        t.table[RetailFixes::kLookControllerConstruct](ctx, 0);
        CHECK(t.body_calls == 1 && t.ram[0x1000] == 0x2f3230 && ctx.cpuRegs.GPR.r[2].UL[0] == 0x1000, "look_body_ran_unchanged");
        CHECK(t.ram[0x1000 + 0x60] == 0, "look_rest_seconds_zeroed");
        t.table[RetailFixes::kLookControllerConstruct](ctx, 0x1473f0);   // a continuation mid-body
        CHECK(fx.Count().look_constructs == 1, "continuation_does_not_reapply");
        ctx.cpuRegs.GPR.r[4].UL[0] = 0x2000;
        t.ram[0x2030] = 0x12345678; t.ram[0x2034] = 0x9abcdef0; t.ram[0x2010] = 7;
        t.table[RetailFixes::kConstructSkidMarks](ctx, 0);
        CHECK(t.ram[0x2010] == 0 && t.ram[0x2014] == 0 && t.ram[0x2018] == 0 && t.ram[0x201c] == 0x3f800000, "skid_last_point_0001");
        CHECK(t.ram[0x2030] == 0x80FFFFFF && t.ram[0x2034] == 0x00FFFFFF, "skid_colours_set");
        CHECK(fx.Census() == "look_rest_seconds=on,applied=1;skid_mark_init=on,applied=1;installed=2", "census_on");
    }
    {
        TestRetailFixes t;
        t.table.erase(RetailFixes::kConstructSkidMarks);
        RetailFixes fx(RetailFixes::Selection{false, true}, t.write32());
        CHECK(!fx.Install(t.capture(), t.installer()) && t.installs == 0, "refused_capture_reports_false");
    }
    if (g_fail) { std::printf("test_retail_fixes: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("test_retail_fixes: all checks passed\n");
    return 0;
}
