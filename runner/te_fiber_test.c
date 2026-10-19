// te_fiber_test.c -- the game coroutine's contract on this host's ABI arm (te_fiber.h): a fixed-address stack with a
// guard below it, the first switch enters fn(arg) on that stack, a switch back resumes the host after its own switch,
// every callee-saved register (integer and vector) survives a round trip in both directions, and the suspended
// context's frame pointer lies inside the stack. One ctest (nativeee.te_fiber); the Windows lane runs the same file over
// te_fiber_win64.c.
#include "te_fiber.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static te_fiber_ctx g_host, g_fiber;
static uint8_t* g_stack;
static int g_failures;
static volatile int g_phase;         /* what the fiber has done so far */
static volatile uint64_t g_seen_arg;
static volatile double g_fiber_sum;  /* the fiber's vector work, read by the host */

#define CHECK(cond) do { if (!(cond)) { ++g_failures; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* Keeps values live across the switch in callee-saved registers (the compiler's choice; the loop makes it keep many). */
static double Work(double seed, int rounds) {
    double a = seed, b = seed * 0.5, c = seed * 0.25, d = seed * 0.125, e = seed * 2.0, f = seed * 4.0;
    for (int i = 0; i < rounds; ++i) {
        a = a * 1.0001 + b; b = b * 0.9999 + c; c = c * 1.0002 + d; d = d * 0.9998 + e; e = e * 1.0003 + f; f = f * 0.9997 + a;
    }
    return a + b + c + d + e + f;
}

static void FiberMain(void* arg) {
    uint8_t probe;   /* a stack object: must live inside the mapped stack */
    g_seen_arg = (uint64_t)(uintptr_t)arg;
    CHECK((uint8_t*)&probe >= g_stack && (uint8_t*)&probe < g_stack + (8u << 20));
    double local = Work(3.0, 1000);        /* live across the yields below */
    uint64_t counter = 0x1122334455667788ull;
    g_phase = 1;
    te_fiber_switch(&g_fiber, &g_host);    /* yield 1 */
    counter += 1;
    local += Work(5.0, 1000);
    g_phase = 2;
    te_fiber_switch(&g_fiber, &g_host);    /* yield 2 */
    counter += 1;
    CHECK(counter == 0x1122334455667788ull + 2);
    g_fiber_sum = local;
    g_phase = 3;
    te_fiber_switch(&g_fiber, &g_host);    /* final: never resumed */
    fprintf(stderr, "  FAIL: a finished fiber was resumed\n");
    ++g_failures;
    for (;;) te_fiber_switch(&g_fiber, &g_host);
}

int main(void) {
    const uint64_t hint = 0x310000000000ull;
    const size_t size = 8u << 20;
    int at_hint = -1;
    g_stack = (uint8_t*)te_fiber_map_stack(hint, size, &at_hint);
    CHECK(g_stack != NULL);
    if (!g_stack) return 1;
    printf("te_fiber_test: stack %p (%s the hint 0x%llx), guard %zu bytes below\n", (void*)g_stack, at_hint ? "at" : "NOT at",
           (unsigned long long)hint, (size_t)te_fiber_guard_bytes);
    /* Writing the top and the bottom byte of the mapping must be fine (the guard is below g_stack). */
    g_stack[0] = 1;
    g_stack[size - 1] = 2;

    te_fiber_prepare(&g_fiber, g_stack, size, FiberMain, (void*)(uintptr_t)0xC0FFEE);
    CHECK(te_fiber_frame_pointer(&g_fiber) == 0);   /* the chain ends at a fresh fiber */

    /* The host's own live state across the switches. */
    double host_local = Work(7.0, 1000);
    uint64_t host_counter = 0xA5A5A5A5A5A5A5A5ull;

    te_fiber_switch(&g_host, &g_fiber);   /* enter */
    CHECK(g_phase == 1);
    CHECK(g_seen_arg == 0xC0FFEE);
    host_counter ^= 1;
    host_local += Work(11.0, 1000);
    {   /* the suspended fiber's registers: its stack pointer and frame pointer lie inside the mapping */
#if defined(__aarch64__) || defined(__arm__)
        const uint64_t sp = g_fiber.sp;
#else
        const uint64_t sp = g_fiber.rsp;
#endif
        const uint64_t lo = (uint64_t)(uintptr_t)g_stack, hi = lo + size;
        CHECK(sp > lo && sp <= hi);
        const uint64_t fp = te_fiber_frame_pointer(&g_fiber);
        CHECK(fp == 0 || (fp >= sp && fp <= hi));   /* a frame pointer when the compiler keeps one */
    }
    te_fiber_switch(&g_host, &g_fiber);   /* resume after yield 1 */
    CHECK(g_phase == 2);
    host_counter ^= 2;
    te_fiber_switch(&g_host, &g_fiber);   /* resume after yield 2 -> the fiber finishes */
    CHECK(g_phase == 3);
    CHECK(host_counter == (0xA5A5A5A5A5A5A5A5ull ^ 3));
    {
        const double expect = Work(7.0, 1000) + Work(11.0, 1000);
        CHECK(host_local == expect);
        const double fiber_expect = Work(3.0, 1000) + Work(5.0, 1000);
        CHECK(g_fiber_sum == fiber_expect);
    }
    te_fiber_unmap_stack(g_stack, size);
    printf("te_fiber_test: %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
