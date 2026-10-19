#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ee/mem/quad_word.h"

namespace frame_oracle {
namespace memory {
extern std::array<uint8_t,0x400000> bytes;
inline uint32_t Address(uint32_t address, size_t size) {
    address &= 0x1fffffff;
    if (address > bytes.size() || size > bytes.size() - address) {
        std::fprintf(stderr,"Frame oracle RAM overrun %08x + %zu\n",address,size);
        std::abort();
    }
    return address;
}
template<class T> T read(uint32_t address) {
    T value;
    std::memcpy(&value,bytes.data()+Address(address,sizeof(T)),sizeof(T));
    return value;
}
template<class T> void write(uint32_t address,const T& value) {
    std::memcpy(bytes.data()+Address(address,sizeof(T)),&value,sizeof(T));
}
inline void write_quad(uint32_t address,const QuadWord& value) { write(address & ~15u,value); }
}
void FUN_0019aaa8(CpuContext&,uint32_t);
void FUN_0019f1c8(CpuContext&,uint32_t);
} // namespace frame_oracle
