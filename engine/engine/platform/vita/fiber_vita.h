// platform/vita/fiber_vita.h -- the PlayStation Vita fiber backend behind the Platform fiber entries and the host
// coroutines: newlib has no <ucontext.h> and the device no user mapping API, so the context switch is an AAPCS32
// register save/load (r4-r11, lr, d8-d15, sp) over a heap stack. Same contract as FiberPosix: one record per fiber plus
// one implicit record per host thread (made on that thread's first Current()).
#pragma once

#include <cstddef>

#include "engine/platform/platform.h"

namespace ps2r::platform {

class FiberVita {
public:
    // A heap stack of `stack` bytes (no guard page: an overflow corrupts the heap instead of faulting) and a context
    // that runs fn(arg) on the first switch. fn must never return.
    static Fiber Create(void (*fn)(void*), void* arg, size_t stack);
    static void Switch(Fiber to);
    static Fiber Current();
    static void Destroy(Fiber f);
};

}  // namespace ps2r::platform
