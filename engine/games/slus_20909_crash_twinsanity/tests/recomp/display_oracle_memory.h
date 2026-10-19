#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <type_traits>

#include "engine/ee/cpu/cpu_state.h"

namespace display_oracle {
namespace memory {
extern std::array<uint8_t,0x400000> bytes;
extern std::function<void(uint32_t,uint64_t)> privileged;
inline uint32_t Address(uint32_t at,size_t size) {
    at &= 0x1fffffff;
    if (at > bytes.size() || size > bytes.size()-at) {
        std::fprintf(stderr,"Display oracle RAM overrun %08x + %zu\n",at,size);
        std::abort();
    }
    return at;
}
template<class T> T read(uint32_t at) {
    T result;
    std::memcpy(&result,bytes.data()+Address(at,sizeof(T)),sizeof(T));
    return result;
}
template<class T> void write(uint32_t at,const T& value) {
    if constexpr (std::is_same_v<T,uint64_t>) {
        if ((at & 0x1fffff00u) == 0x12000000u && privileged) {
            privileged(at,value);
            return;
        }
    }
    std::memcpy(bytes.data()+Address(at,sizeof(T)),&value,sizeof(T));
}
} // namespace memory
void FUN_0019aff0(CpuContext&,uint32_t);
} // namespace display_oracle
