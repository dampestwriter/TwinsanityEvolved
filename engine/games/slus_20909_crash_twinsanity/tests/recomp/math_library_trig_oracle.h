#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ipu/hle/guest_memory.h"

class MathLibraryTrigOracle {
public:
    class Memory final : public ps2r::ipu::GuestMemory {
    public:
        std::array<uint8_t, 8192> bytes{};
        uint8_t* Translate(uint32_t address, size_t length) override;
        uint32_t Read32(uint32_t address) override;
        void Write32(uint32_t address, uint32_t value) override;
    };
    // The test-only NEW model provider calls the existing production shortcut,
    // under its production rounding guard. The replacement never calls it.
    class Model {
    public:
        static void Execute(CpuContext& context, uint32_t address);
        static uint64_t Calls();
        static void ResumeIfWaiting(CpuContext& context);
    };
    class UnexpectedCall {
    public:
        static void Execute(CpuContext& context, uint32_t address);
    };
    struct Entry {
        const char* name;
        uint32_t address;
    };
    static constexpr unsigned kEntryCount = 31;
    static constexpr unsigned kCasesPerEntry = 4000;
    static const std::array<Entry, kEntryCount>& Entries();
    static const char* SourceDigest();
    using Dispatch = std::function<void(uint32_t, CpuContext&)>;
    // Counts entry to each extracted NEW body, including nested calls.
    static std::array<uint64_t, kEntryCount> BodyCalls();
    // An installed control arm routes nested bound callees through its actual
    // installed wrappers. Direct reference execution uses only NEW bodies.
    static void Invoke(uint32_t address, CpuContext& context, Memory& memory, const Dispatch& dispatch = {});
};
