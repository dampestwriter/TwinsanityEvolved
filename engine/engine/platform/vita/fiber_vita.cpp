// platform/vita/fiber_vita.cpp -- AAPCS32 fibers for the PlayStation Vita (armv7-a, Thumb-2, VFPv3-D32 + NEON).
// Callee-saved state is r4-r11, lr and d8-d15; sp is 8-byte aligned at a function's entry. The context layout below is
// the switch's: sp at 0, r4..r11 + lr at 4..36 (9 words), d8..d15 at 40.
#include "engine/platform/vita/fiber_vita.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2r::platform {
namespace {

struct FiberContext {
    uint32_t sp;
    uint32_t r[9];   // r4..r11, lr
    uint64_t d[8];   // d8..d15
};
static_assert(offsetof(FiberContext, r) == 4 && offsetof(FiberContext, d) == 40, "FiberContext: the switch's offsets");

struct FiberRecord {
    FiberContext ctx{};
    uint8_t*     stack = nullptr;   // null for a host thread's own record
    void       (*entry)(void*) = nullptr;
    void*        arg = nullptr;
};

thread_local FiberRecord* t_current = nullptr;

FiberRecord* HostRecord() {
    if (!t_current) t_current = new FiberRecord();
    return t_current;
}

}  // namespace
}  // namespace ps2r::platform

extern "C" void ps2r_vita_fiber_switch(ps2r::platform::FiberContext* save, const ps2r::platform::FiberContext* load);
extern "C" void ps2r_vita_fiber_trampoline();

// save into *r0, load *r1, return into the loaded lr
asm(R"(
    .syntax unified
    .thumb
    .text
    .balign 4
    .globl ps2r_vita_fiber_switch
    .type ps2r_vita_fiber_switch, %function
    .thumb_func
ps2r_vita_fiber_switch:
    str     sp, [r0, #0]
    add     r2, r0, #4
    stmia   r2!, {r4-r11, lr}
    add     r2, r0, #40
    vstmia  r2, {d8-d15}
    ldr     sp, [r1, #0]
    add     r2, r1, #4
    ldmia   r2!, {r4-r11, lr}
    add     r2, r1, #40
    vldmia  r2, {d8-d15}
    bx      lr
    .size ps2r_vita_fiber_switch, .-ps2r_vita_fiber_switch

    .globl ps2r_vita_fiber_trampoline
    .type ps2r_vita_fiber_trampoline, %function
    .thumb_func
ps2r_vita_fiber_trampoline:
    mov     r0, r5
    blx     r4
    udf     #0
    .size ps2r_vita_fiber_trampoline, .-ps2r_vita_fiber_trampoline
)");

namespace ps2r::platform {
namespace {

// First entry of a fiber: the trampoline calls Entry(record) with r4 = Entry, r5 = the record.
void Entry(void* p) {
    auto* r = static_cast<FiberRecord*>(p);
    r->entry(r->arg);
    std::fprintf(stderr, "platform: fiber entry returned; a fiber body must park forever\n");
    std::abort();
}

}  // namespace

Fiber FiberVita::Create(void (*fn)(void*), void* arg, size_t stack) {
    Fiber f;
    if (!fn) return f;
    if (stack < 64 * 1024) stack = 64 * 1024;
    stack = (stack + 15) & ~static_cast<size_t>(15);
    auto* r = new FiberRecord();
    r->stack = static_cast<uint8_t*>(std::malloc(stack));
    if (!r->stack) {
        delete r;
        return f;
    }
    r->entry = fn;
    r->arg = arg;
    const uintptr_t top = (reinterpret_cast<uintptr_t>(r->stack) + stack) & ~static_cast<uintptr_t>(15);
    r->ctx.sp = static_cast<uint32_t>(top);
    r->ctx.r[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&Entry));                       // r4
    r->ctx.r[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(r));                            // r5
    r->ctx.r[7] = 0;                                                                                // r11: the frame chain ends here
    r->ctx.r[8] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ps2r_vita_fiber_trampoline));  // lr (Thumb bit set)
    f.handle = r;
    return f;
}

void FiberVita::Switch(Fiber to) {
    auto* target = static_cast<FiberRecord*>(to.handle);
    if (!target) return;
    FiberRecord* from = HostRecord();
    if (from == target) return;
    t_current = target;
    ps2r_vita_fiber_switch(&from->ctx, &target->ctx);
    // Resumed: whoever switched back to us set t_current before switching.
    t_current = from;
}

Fiber FiberVita::Current() {
    Fiber f;
    f.handle = HostRecord();
    return f;
}

void FiberVita::Destroy(Fiber f) {
    auto* r = static_cast<FiberRecord*>(f.handle);
    if (!r || !r->stack) return;   // host records are never destroyed
    std::free(r->stack);
    delete r;
}

}  // namespace ps2r::platform
