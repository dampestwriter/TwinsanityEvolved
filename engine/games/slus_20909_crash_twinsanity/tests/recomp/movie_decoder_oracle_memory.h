#pragma once
// Test-local transport for unchanged NEW generated movie bodies. IPU MMIO
// and SDK calls reach the real NEW IpuDevice; no command counter is stubbed.
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ee/mem/quad_word.h"

namespace movie_oracle {
void Read(uint32_t address, void* destination, size_t size);
void Write(uint32_t address, const void* source, size_t size);
void Dispatch(uint32_t address, CpuContext& context);
bool RunGenerated(uint32_t address, CpuContext& context);
void ObserveGenerated(uint32_t address);
namespace memory {
template<class T> T read(uint32_t address);
template<class T> void write(uint32_t address, const T& value) {
    Write(address, &value, sizeof(value));
}
inline void read_quad(uint32_t address, QuadWord& value) {
    value = read<QuadWord>(address & ~15u);
}
inline void write_quad(uint32_t address, const QuadWord& value) {
    write(address & ~15u, value);
}
}  // namespace memory
// Generated diagnostics remain compiled and inert on valid test inputs.
extern std::ostringstream g_logFile;
inline constexpr bool g_recomp_corpus_enabled = false, crashring_enabled = false;
inline void recomp_corpus_record(uint32_t, uint32_t) {}
inline bool ee_get_call_stack_top(uint32_t, uint32_t*, uint32_t*) { return false; }
inline void crashring_record_indirect(uint32_t, uint32_t) {}
const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t);
inline void recomp_indirect_call(const std::function<void(CpuContext&, uint32_t)>* call,
                                uint32_t address, CpuContext& context) {
    (*call)(context, address);
}
}  // namespace movie_oracle

#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.inl"
