#pragma once
// Generated-code ABI forwarder: the `iop_memory::` free functions the generated
// loads/stores call, routed to the IopMemory bound through StubSurface.
#include <cstdint>

#include "engine/iop/iop_memory.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/abi/stub_surface.h"

namespace iop_memory {

template <typename T>
inline T read(uint32_t address) {
    return ps2r::title::StubSurface::Memory().Read<T>(address);
}

template <typename T>
inline void write(uint32_t address, T value) {
    ps2r::title::StubSurface::Memory().Write<T>(address, value);
}

}  // namespace iop_memory
