// te_fiber.c -- te_fiber_prepare and the fixed-address stack mapping (see te_fiber.h; the switch is te_fiber_switch.S
// on POSIX, te_fiber_win64.c on Windows).
#include "te_fiber.h"

#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__vita__)
#include <stdlib.h>   /* no mmap on the device: the stack is a heap block (no guard page; the hint is never honoured) */
#else
#include <sys/mman.h>
#endif

extern void te_fiber_trampoline(void);

/* The asm's offsets (te_fiber_switch.S / te_fiber_win64.c) are the struct's. */
#if defined(_WIN64)
_Static_assert(offsetof(te_fiber_ctx, rip) == 72, "te_fiber_ctx: rip at 72 (te_fiber_win64.c)");
_Static_assert(offsetof(te_fiber_ctx, xmm) == 80, "te_fiber_ctx: xmm6 at 80 (te_fiber_win64.c)");
_Static_assert(offsetof(te_fiber_ctx, tib) == 240, "te_fiber_ctx: TIB words at 240 (te_fiber_win64.c)");
_Static_assert(sizeof(te_fiber_ctx) == 264, "te_fiber_ctx: 33 words");
#elif defined(__aarch64__)
_Static_assert(offsetof(te_fiber_ctx, x) == 8 && offsetof(te_fiber_ctx, d) == 104, "te_fiber_ctx: x19 at 8, d8 at 104 (te_fiber_switch.S)");
#elif defined(__x86_64__)
_Static_assert(offsetof(te_fiber_ctx, rip) == 56, "te_fiber_ctx: rip at 56 (te_fiber_switch.S)");
#elif defined(__arm__)
_Static_assert(offsetof(te_fiber_ctx, r) == 4 && offsetof(te_fiber_ctx, d) == 40, "te_fiber_ctx: r4 at 4, d8 at 40 (te_fiber_switch.S)");
#endif

void te_fiber_prepare(te_fiber_ctx* ctx, void* stack, size_t size, void (*fn)(void*), void* arg) {
    memset(ctx, 0, sizeof *ctx);
    uint64_t top = ((uint64_t)(uintptr_t)stack + size) & ~(uint64_t)15;
#if defined(_WIN64)
    // The Windows x64 ABI: at a function's entry rsp is 16-aligned MINUS 8 (the return address) and the caller owns a
    // 32-byte shadow space above it. The trampoline is entered by `ret` with rsp = top - 8 - 32 ... see te_fiber_win64.c:
    // it calls fn with its own shadow space, so the prepared rsp only needs to be 16-aligned here.
    ctx->rsp = top;
    ctx->r12 = (uint64_t)(uintptr_t)fn;
    ctx->r13 = (uint64_t)(uintptr_t)arg;
    ctx->rbp = 0;
    ctx->rip = (uint64_t)(uintptr_t)te_fiber_trampoline;
    ctx->tib[0] = top;                               /* StackBase */
    ctx->tib[1] = (uint64_t)(uintptr_t)stack;        /* StackLimit: the lowest committed byte (the guard is below) */
    ctx->tib[2] = (uint64_t)(uintptr_t)stack;        /* DeallocationStack: never freed through the thread; kept inside the
                                                        region so a moved mapping relocates it with the rest */
#elif defined(__aarch64__)
    ctx->sp = top;
    ctx->x[0] = (uint64_t)(uintptr_t)fn;    /* x19 */
    ctx->x[1] = (uint64_t)(uintptr_t)arg;   /* x20 */
    ctx->x[10] = 0;                          /* x29: the fp chain ends here */
    ctx->x[11] = (uint64_t)(uintptr_t)te_fiber_trampoline;   /* x30: `ret` lands in the trampoline */
#elif defined(__x86_64__)
    ctx->rsp = top;
    ctx->r12 = (uint64_t)(uintptr_t)fn;
    ctx->r13 = (uint64_t)(uintptr_t)arg;
    ctx->rbp = 0;
    ctx->rip = (uint64_t)(uintptr_t)te_fiber_trampoline;
#elif defined(__arm__)
    ctx->sp = (uint32_t)top;                               /* 8-byte aligned at the trampoline's entry (AAPCS) */
    ctx->r[0] = (uint32_t)(uintptr_t)fn;                    /* r4 */
    ctx->r[1] = (uint32_t)(uintptr_t)arg;                   /* r5 */
    ctx->r[7] = 0;                                          /* r11 (fp): the frame chain ends here */
    ctx->r[8] = (uint32_t)(uintptr_t)te_fiber_trampoline;   /* lr: `bx lr` lands in the trampoline (Thumb bit set) */
#endif
}

uint64_t te_fiber_frame_pointer(const te_fiber_ctx* ctx) {
#if defined(__aarch64__)
    return ctx->x[10];
#elif defined(__arm__)
    return ctx->r[7];
#else
    return ctx->rbp;
#endif
}

void* te_fiber_map_stack(uint64_t hint, size_t size, int* at_hint) {
    const size_t guard = te_fiber_guard_bytes;
    void* want = (void*)(uintptr_t)(hint - guard);   /* a 32-bit target truncates the 48-bit hint; its arm below ignores it */
#if defined(_WIN32)
    // Reserve + commit in one step (the commit is a charge, no page is touched until the game runs); the guard below the
    // stack is made inaccessible so an overflow faults instead of writing below the mapping (as the POSIX arm's PROT_NONE).
    void* p = VirtualAlloc(want, size + guard, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        p = VirtualAlloc(NULL, size + guard, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) return NULL;
    }
    DWORD old = 0;
    VirtualProtect(p, guard, PAGE_NOACCESS, &old);
#elif defined(__vita__)
    /* A heap block: the device has no user mapping API, so no guard page (an overflow corrupts the heap instead of
       faulting) and never at the hint (the save states that depend on it are not built for the device). */
    (void)want;
    void* p = malloc(size + guard);
    if (!p) return NULL;
    want = NULL;
#else
    void* p = mmap(want, size + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        p = mmap(NULL, size + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return NULL;
    }
    mprotect(p, guard, PROT_NONE);
#endif
    if (at_hint) *at_hint = (p == want);
    return (char*)p + guard;
}

void te_fiber_unmap_stack(void* stack, size_t size) {
    const size_t guard = te_fiber_guard_bytes;
    if (!stack) return;
#if defined(_WIN32)
    (void)size;
    VirtualFree((char*)stack - guard, 0, MEM_RELEASE);
#elif defined(__vita__)
    (void)size;
    free((char*)stack - guard);
#else
    munmap((char*)stack - guard, size + guard);
#endif
}
