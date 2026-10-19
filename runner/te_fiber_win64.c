// te_fiber_win64.c -- the coroutine switch for the Windows x64 ABI (see te_fiber.h; the POSIX arms are
// te_fiber_switch.S). A naked function with GNU-syntax inline asm: clang and clang-cl assemble it inside the C
// compile, so the Windows lane needs no separate assembler step. cl.exe cannot build this file (no naked x64
// functions, no inline asm) -- the port's Windows lane is clang-cl.
//
// Stores the callee-saved registers of the running fiber into *save (rcx) and loads the fiber in *load (rdx). The
// register layout is te_fiber_ctx's (_WIN64 arm):
//    0 rsp    8 rbx   16 rbp   24 rsi   32 rdi   40 r12   48 r13   56 r14   64 r15   72 rip
//   80 xmm6 .. 224 xmm15 (16 bytes each, unaligned stores: the context lives inside a plain struct)
//  240 NT_TIB.StackBase (gs:0x08)   248 NT_TIB.StackLimit (gs:0x10)   256 DeallocationStack (gs:0x1478)
// The thread's stack bounds travel with the stack: structured exception dispatch and the CRT's stack probes read them
// (a fiber running on a stack outside the TIB's bounds is what RtlDispatchException fast-fails on).
#if defined(_WIN64) && (defined(__clang__) || defined(__GNUC__))

#include "te_fiber.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
__attribute__((naked)) void te_fiber_switch(te_fiber_ctx* save, const te_fiber_ctx* load) {
    __asm__ volatile(
        /* ---- save (rcx) ---- */
        "movq    (%rsp), %rax\n\t"          /* the return address */
        "movq    %rax, 72(%rcx)\n\t"
        "leaq    8(%rsp), %rax\n\t"         /* the caller's rsp after our return */
        "movq    %rax, 0(%rcx)\n\t"
        "movq    %rbx, 8(%rcx)\n\t"
        "movq    %rbp, 16(%rcx)\n\t"
        "movq    %rsi, 24(%rcx)\n\t"
        "movq    %rdi, 32(%rcx)\n\t"
        "movq    %r12, 40(%rcx)\n\t"
        "movq    %r13, 48(%rcx)\n\t"
        "movq    %r14, 56(%rcx)\n\t"
        "movq    %r15, 64(%rcx)\n\t"
        "movups  %xmm6,  80(%rcx)\n\t"
        "movups  %xmm7,  96(%rcx)\n\t"
        "movups  %xmm8,  112(%rcx)\n\t"
        "movups  %xmm9,  128(%rcx)\n\t"
        "movups  %xmm10, 144(%rcx)\n\t"
        "movups  %xmm11, 160(%rcx)\n\t"
        "movups  %xmm12, 176(%rcx)\n\t"
        "movups  %xmm13, 192(%rcx)\n\t"
        "movups  %xmm14, 208(%rcx)\n\t"
        "movups  %xmm15, 224(%rcx)\n\t"
        "movq    %gs:0x08, %rax\n\t"
        "movq    %rax, 240(%rcx)\n\t"
        "movq    %gs:0x10, %rax\n\t"
        "movq    %rax, 248(%rcx)\n\t"
        "movq    %gs:0x1478, %rax\n\t"
        "movq    %rax, 256(%rcx)\n\t"
        /* ---- load (rdx) ---- */
        "movq    240(%rdx), %rax\n\t"
        "movq    %rax, %gs:0x08\n\t"
        "movq    248(%rdx), %rax\n\t"
        "movq    %rax, %gs:0x10\n\t"
        "movq    256(%rdx), %rax\n\t"
        "movq    %rax, %gs:0x1478\n\t"
        "movq    0(%rdx), %rsp\n\t"
        "movq    8(%rdx), %rbx\n\t"
        "movq    16(%rdx), %rbp\n\t"
        "movq    24(%rdx), %rsi\n\t"
        "movq    32(%rdx), %rdi\n\t"
        "movq    40(%rdx), %r12\n\t"
        "movq    48(%rdx), %r13\n\t"
        "movq    56(%rdx), %r14\n\t"
        "movq    64(%rdx), %r15\n\t"
        "movups  80(%rdx),  %xmm6\n\t"
        "movups  96(%rdx),  %xmm7\n\t"
        "movups  112(%rdx), %xmm8\n\t"
        "movups  128(%rdx), %xmm9\n\t"
        "movups  144(%rdx), %xmm10\n\t"
        "movups  160(%rdx), %xmm11\n\t"
        "movups  176(%rdx), %xmm12\n\t"
        "movups  192(%rdx), %xmm13\n\t"
        "movups  208(%rdx), %xmm14\n\t"
        "movups  224(%rdx), %xmm15\n\t"
        "movq    72(%rdx), %rax\n\t"
        "jmpq    *%rax\n\t");
}
#pragma clang diagnostic pop

// First entry of a prepared fiber: r12 = fn, r13 = arg (te_fiber_prepare); rsp = the 16-aligned stack top. The call
// needs the 32-byte shadow space and rsp == 8 (mod 16) at fn's entry: sub 32 keeps the alignment, the call's push
// supplies the 8. fn never returns.
__attribute__((naked)) void te_fiber_trampoline(void) {
    __asm__ volatile(
        "subq    $32, %rsp\n\t"
        "movq    %r13, %rcx\n\t"
        "callq   *%r12\n\t"
        "ud2\n\t");
}

#else
// Not the Windows x64 arm: te_fiber_switch.S carries the switch (CMakeLists.txt lists one or the other).
typedef int te_fiber_win64_not_built;
#endif
