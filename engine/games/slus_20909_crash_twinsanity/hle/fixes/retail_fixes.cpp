#include "games/slus_20909_crash_twinsanity/hle/fixes/retail_fixes.h"

#include "engine/ee/cpu/cpu_state.h"

namespace title {

RetailFixes::RetailFixes(Selection selection, Write32 write32) : selection_(selection), write32_(std::move(write32)) {}

bool RetailFixes::Install(const Capture& capture, const Installer& install) {
    if (!capture || !install || !write32_) return !selection_.look_rest_seconds && !selection_.skid_mark_init;
    bool ok = true;
    if (selection_.look_rest_seconds) {
        Body original = capture(kLookControllerConstruct);
        if (!original) ok = false;
        else if (install(kLookControllerConstruct, [this, original](CpuContext& ctx, uint32_t addr) {
                     const bool entry = addr == 0 || addr == kLookControllerConstruct;
                     original(ctx, addr);
                     if (!entry) return;   // a continuation resumed mid-body: the constructor's own return path
                     // v0 = the look (the constructor returns its argument).
                     const uint32_t look = ctx.cpuRegs.GPR.r[2].UL[0];
                     if (look) { write32_(look + kRestSecondsOffset, 0u); ++counts_.look_constructs; }
                 }))
            ++counts_.installed;
        else ok = false;
    }
    if (selection_.skid_mark_init) {
        Body original = capture(kConstructSkidMarks);
        if (!original) ok = false;
        else if (install(kConstructSkidMarks, [this, original](CpuContext& ctx, uint32_t addr) {
                     const bool entry = addr == 0 || addr == kConstructSkidMarks;
                     const uint32_t marks = ctx.cpuRegs.GPR.r[4].UL[0];   // a0 at entry
                     original(ctx, addr);
                     if (!entry || !marks) return;
                     write32_(marks + kLastPointOffset + 0x0, 0u);
                     write32_(marks + kLastPointOffset + 0x4, 0u);
                     write32_(marks + kLastPointOffset + 0x8, 0u);
                     write32_(marks + kLastPointOffset + 0xc, 0x3f800000u);   // w = 1.0f
                     write32_(marks + kEdgeColourOffset, 0x80FFFFFFu);
                     write32_(marks + kLowColourOffset, 0x00FFFFFFu);
                     ++counts_.skid_constructs;
                 }))
            ++counts_.installed;
        else ok = false;
    }
    return ok;
}

std::string RetailFixes::Census() const {
    return std::string("look_rest_seconds=") + (selection_.look_rest_seconds ? "on" : "off") + ",applied=" +
           std::to_string(counts_.look_constructs) + ";skid_mark_init=" + (selection_.skid_mark_init ? "on" : "off") +
           ",applied=" + std::to_string(counts_.skid_constructs) + ";installed=" + std::to_string(counts_.installed);
}

}  // namespace title
