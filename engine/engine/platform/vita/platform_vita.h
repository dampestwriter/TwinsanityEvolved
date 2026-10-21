// platform/vita/platform_vita.h -- the PlayStation Vita Platform (VitaSDK, newlib, armv7-a): steady-clock time,
// pthreads (the SDK's pthread over the kernel's threads) ranked by the kernel's priorities, stdio files, 4 KB pages,
// the AAPCS32 fibers (fiber_vita), the kernel's free-memory report as the footprint and app0:eboot.bin as the image.
// Reached through engine/platform/host/platform.h only; surface/input/audio arrive with the SDL folder (SDL2's Vita port).
#pragma once

#include <cstdint>
#include <string>

#include "engine/platform/platform.h"

namespace ps2r::platform {

class PlatformVita {
public:
    static const Platform& Instance();
    static void RegisterSelf();

    // The kernel priority by role (a lower number runs first; a user thread may use 64..191 on the device, Vita3K
    // refuses 64 itself): Pacer 72, Game 96, Graphics 128, Worker 144, Interactive 160. The name is the kernel's own.
    static void PinCurrentThreadInteractive(const char* name, ThreadRole role = ThreadRole::Interactive);
    // The part of the application's memory grant in use (the grant minus sceKernelGetFreeMemorySize's free total).
    static uint64_t PhysFootprintBytes();
    static std::string ExecutablePath(std::string* err);   // "app0:eboot.bin"
};

}  // namespace ps2r::platform
