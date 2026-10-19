#pragma once
// Test-only RAM for extracted NEW generated bodies. No device, retail image,
// external recording or process-wide runtime memory is used by this oracle.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>

#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include "engine/ee/mem/quad_word.h"

namespace strip_oracle {
namespace memory {
extern std::array<uint8_t, 0x400000> bytes;
inline uint32_t Address(uint32_t address, size_t size) {
    address &= 0x1fffffffu;
    if (address > bytes.size() || size > bytes.size() - address) {
        std::fprintf(stderr, "oracle RAM overrun: %08x + %zu\n", address, size);
        std::abort();
    }
    return address;
}
template<class T> T read(uint32_t address) {
    T value;
    std::memcpy(&value, bytes.data() + Address(address, sizeof(T)), sizeof(T));
    return value;
}
template<class T> void write(uint32_t address, const T& value) {
    std::memcpy(bytes.data() + Address(address, sizeof(T)), &value, sizeof(T));
}
inline void read_quad(uint32_t address, QuadWord& value) {
    value = read<QuadWord>(address & ~15u);
}
inline void write_quad(uint32_t address, const QuadWord& value) {
    write(address & ~15u, value);
}
}  // namespace memory
void RunGenerated(uint32_t address, CpuContext& context);
void ObserveGenerated(uint32_t address);
void ExternalCall(uint32_t address, CpuContext& context);
// Generated indirect-call transport, with test-local lifetime-stable lookup.
// Logging/corpus probes remain compiled but are disabled on valid fixture input.
inline std::ostringstream g_logFile;
inline constexpr bool g_recomp_corpus_enabled = false, crashring_enabled = false;
inline void recomp_corpus_record(uint32_t, uint32_t) {}
inline bool ee_get_call_stack_top(uint32_t, uint32_t*, uint32_t*) { return false; }
inline void crashring_record_indirect(uint32_t, uint32_t) {}
inline const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t) {
    static const std::function<void(CpuContext&, uint32_t)> call =
        [](CpuContext& context, uint32_t address) { RunGenerated(address, context); };
    return &call;
}
inline void recomp_indirect_call(const std::function<void(CpuContext&, uint32_t)>* call,
                                uint32_t address, CpuContext& context) {
    (*call)(context, address);
}
}  // namespace strip_oracle
