// platform/nativeee (wasm side) -- the module's entry, the retail crt0's job (game/src/platform/ps2/entry.cpp) without a
// PS2: the retail .bss is zero because the runner loads the image into zeroed memory; the main thread is the one game
// thread; the heap's break starts past the C++'s data and shadow stack.
#include "common.h"
#include "platform/system.h"
#include "retail/libc.h"
#include "ps2e_seam.h"
#include <kernel.h>

extern "C" unsigned char __heap_base[];

extern "C"
{
    int Main(u32 argc, char** argv);

    // The game's entry, exported from the module and called ONCE by the runner after the retail image is loaded and its
    // code pointers patched (tools/gen_retail_table.py). Never returns normally: Main ends in Platform::System::Exit.
    __attribute__((export_name("entry"))) void entry(void)
    {
        // The retail break word holds the retail executable's end (0x3DB200-ish): move it past our data and stack
        u32 now = reinterpret_cast<u32>(RetailLibc::Sbrk(0));
        u32 base = reinterpret_cast<u32>(__heap_base);
        if (now < base)
        {
            RetailLibc::Sbrk(static_cast<s32>((base - now + 15u) & ~15u));
        }

        static char* argv[] = {const_cast<char*>("twinsanityevolved"), nullptr};
        Platform::System::Exit(Main(1, argv));
    }

    // The retail crt0's exit (FUN_001000e0) called it on the PS2; kept for the symbol
    [[noreturn]] void ProgramExit(int status)
    {
        Platform::System::Exit(status);
    }

    int kputs(char* text)
    {
        ps2e_system_kputs(reinterpret_cast<ps2e_u32>(text));
        return 0;
    }
}
