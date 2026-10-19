// PS2SDK <kernel.h> shim for the wasm32 game module. Only what the KEPT decomp files use
// (game/src/platform/ps2/{libc,malloc}.cpp): interrupt gating, the heap's end, the kernel TTY.
// One EE thread, no interrupts in the module: DIntr/EIntr are no-ops that report "was enabled".
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline int DIntr(void) { return 1; }
static inline int EIntr(void) { return 1; }
// The end of the memory Sbrk may hand out: the top of the 32 MB (the retail SetupHeap(_end, -1)); the wasm
// shadow stack and the C++'s own data live BELOW the retail image, so the heap owns everything above it.
void* EndOfHeap(void);
// Sony's kernel TTY: a line to the host log (seam import ps2e_system_kputs).
int kputs(char* text);
#ifdef __cplusplus
}
#endif
