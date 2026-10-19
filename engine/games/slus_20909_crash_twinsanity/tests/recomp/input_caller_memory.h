#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <sstream>

#include "engine/ee/cpu/cpu_state.h"

namespace input_callers {
void Read(uint32_t, void*, size_t);
void Write(uint32_t, const void*, size_t);
void Dispatch(uint32_t, CpuContext&);
bool Run(uint32_t, CpuContext&);
namespace memory {
template<class T> T read(uint32_t address) {
    T value;
    Read(address, &value, sizeof(value));
    return value;
}
template<class T> void write(uint32_t address, const T& value) {
    Write(address, &value, sizeof(value));
}
}
extern std::ostringstream g_logFile;
inline constexpr bool g_recomp_corpus_enabled = false, crashring_enabled = false;
inline void recomp_corpus_record(uint32_t, uint32_t) {}
inline bool ee_get_call_stack_top(uint32_t, uint32_t*, uint32_t*) { return false; }
inline void crashring_record_indirect(uint32_t, uint32_t) {}
const std::function<void(CpuContext&, uint32_t)>* find_containing_function(uint32_t);
inline void recomp_indirect_call(const std::function<void(CpuContext&, uint32_t)>* body,
                                uint32_t address, CpuContext& context) {
    (*body)(context, address);
}
namespace ps2_threading { [[noreturn]] void exitThread(); }
}
