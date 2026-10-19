// te_fiber.h -- the game coroutine: the wasm2c module runs on its OWN native stack at a FIXED address, so the stack
// (and the callee-saved register file at the switch point) is a plain byte region a save state can carry and a later
// process can put back at the same address. One switch each way per save/load; the host
// (runner) side never runs game code.
//
// Portable-by-construction: te_fiber_switch.S holds the AAPCS64 (arm64) and SysV x86-64 switch; te_fiber_win64.c holds
// the Windows x64 switch (a naked function with GNU-syntax inline asm -- clang and clang-cl, no assembler step: the port's
// Windows lane is clang-cl). The stack mapping is mmap (POSIX) or VirtualAlloc (Win32) at the same hint.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The callee-saved register file at a switch point (what te_fiber_switch stores/loads). Layout is the asm's. Every
// field is a 64-bit word: the save state relocates the context word by word (te_savestate.cpp Relocate).
typedef struct te_fiber_ctx {
#if defined(_WIN64)
    uint64_t rsp;
    uint64_t rbx, rbp, rsi, rdi, r12, r13, r14, r15;
    uint64_t rip;
    uint64_t xmm[20];   /* xmm6..xmm15, two words each (the Windows x64 ABI preserves all 128 bits) */
    uint64_t tib[3];    /* NT_TIB StackBase, StackLimit, DeallocationStack: the thread's stack bounds, swapped with the stack */
#elif defined(__aarch64__)
    uint64_t sp;
    uint64_t x[12];   /* x19..x28, x29 (fp), x30 (lr) */
    uint64_t d[8];    /* d8..d15 */
#elif defined(__x86_64__)
    uint64_t rsp;
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rip;
#elif defined(__arm__)
    /* AAPCS32 (the PlayStation Vita's armv7-a / Thumb-2): 32-bit words. */
    uint32_t sp;
    uint32_t r[9];    /* r4..r11, lr */
    uint64_t d[8];    /* d8..d15 */
#else
#error "te_fiber: unsupported architecture"
#endif
} te_fiber_ctx;

// Saves the current callee-saved state into *save and continues in *load. Returns when someone switches back to *save.
void te_fiber_switch(te_fiber_ctx* save, const te_fiber_ctx* load);

// Prepares *ctx so that the first te_fiber_switch(..., ctx) calls fn(arg) on the stack [stack, stack + size). fn must
// never return (it switches away for good); the trampoline traps if it does.
void te_fiber_prepare(te_fiber_ctx* ctx, void* stack, size_t size, void (*fn)(void*), void* arg);

// A stack mapping at a fixed address (the hint; a different address is accepted and reported through *at_hint = 0) with
// a no-access guard region (te_fiber_guard_bytes) below it. NULL on failure.
void* te_fiber_map_stack(uint64_t hint, size_t size, int* at_hint);
void te_fiber_unmap_stack(void* stack, size_t size);
// The guard region's size (64 KB: Windows' allocation granularity, a multiple of every page size we run on).
#define te_fiber_guard_bytes ((size_t)65536)

// The frame pointer of a suspended context (the saved-fp / return-address chain starts here): the save state's verbose
// stack walk.
uint64_t te_fiber_frame_pointer(const te_fiber_ctx* ctx);

#ifdef __cplusplus
}
#endif
