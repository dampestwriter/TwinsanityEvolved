#pragma once
#include <array>
#include <atomic>   // the generated shard excerpts (generate_rigid_content_oracle.py) use std::atomic; libstdc++ does not include it transitively
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>

#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include "engine/ee/mem/quad_word.h"

namespace rigid_content_oracle {
namespace memory {
extern std::array<uint8_t, 0x800000> bytes;
inline uint32_t Address(uint32_t address, size_t size) {
    address &= 0x1fffffff;
    if (address > bytes.size() || size > bytes.size() - address) std::abort();
    return address;
}
template<class T> T read(uint32_t address) {
    T out;
    std::memcpy(&out, bytes.data() + Address(address, sizeof(T)), sizeof(T));
    return out;
}
template<class T> void write(uint32_t address, const T& value) {
    std::memcpy(bytes.data() + Address(address, sizeof(T)), &value, sizeof(T));
}
inline void read_quad(uint32_t address, QuadWord& out) { out = read<QuadWord>(address & ~15u); }
inline void write_quad(uint32_t address, const QuadWord& value) { write(address & ~15u, value); }
}  // namespace memory
void Run(uint32_t address, CpuContext& context);
void Original(uint32_t address, CpuContext& context);
void External(uint32_t address, CpuContext& context);
// Transport only. The two type-01 virtual targets dispatch to their actual
// extracted generated bodies. Bad-source logging is disabled on valid inputs.
inline std::ofstream g_logFile;
// Complete generated billboard/math bodies contain these calls. Billboards
// are outside this selected family. Reaching either is a failed fixture/gate,
// never a substituted microprogram or an invented producer result.
struct UnexpectedProgramCall {
    [[noreturn]] void Execute(CpuContext&, uint32_t) { std::abort(); }
    [[noreturn]] void ResumeIfWaiting(CpuContext&) { std::abort(); }
};
inline UnexpectedProgramCall g_vu0, g_vu1;
inline constexpr bool g_recomp_corpus_enabled = false, crashring_enabled = false;
inline void recomp_corpus_record(uint32_t, uint32_t) {}
inline bool ee_get_call_stack_top(uint32_t, uint32_t*, uint32_t*) { return false; }
inline void crashring_record_indirect(uint32_t, uint32_t) {}
inline const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t) {
    static const std::function<void(CpuContext&, uint32_t)> call =
        [](CpuContext& context, uint32_t address) { Run(address, context); };
    return &call;
}
inline void recomp_indirect_call(const std::function<void(CpuContext&, uint32_t)>* call,
                                uint32_t address, CpuContext& context) { (*call)(context, address); }
}  // namespace rigid_content_oracle
