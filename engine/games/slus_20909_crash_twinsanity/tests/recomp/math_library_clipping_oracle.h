#pragma once

#include <array>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

class MathLibraryClippingOracle {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        std::array<uint8_t, 4096> bytes{};
        uint8_t* Translate(uint32_t address, size_t length) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
    };
    // The actual captured control arm supplies its installed ClipCorners body
    // so the source tail call contributes to that library's original counters.
    static void Invoke(uint32_t entry, CpuContext& context, Memory& memory,
                       uint32_t dispatch = 0, const Body* corners = nullptr);
    static std::array<uint64_t, 2> Counts();
    static const char* SourceDigest();
};
