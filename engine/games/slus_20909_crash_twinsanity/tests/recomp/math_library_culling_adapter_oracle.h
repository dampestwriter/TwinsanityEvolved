#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

class VuUnit;

// Actual generated boundary bodies and actual NEW culling arithmetic.
// Upload/selection are observable test seams, not executed hardware models.
class MathLibraryCullingAdapterOracle {
public:
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        Memory();
        uint8_t* Translate(uint32_t address, size_t size) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
        std::vector<uint8_t> bytes;
    };
    struct Counters {
        std::map<uint32_t, uint64_t> generated_bodies;
        uint64_t actual_culling_executions;
        uint64_t upload_requests;
        uint64_t selection_requests;
    };
    static void Invoke(uint32_t address, CpuContext& context, Memory& memory, VuUnit& control);
    static Counters Counts();
    static const char* SourceDigest();
};
