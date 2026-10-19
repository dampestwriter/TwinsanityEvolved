#pragma once
// Private RAM for the actual NEW generated FinishSky body, test only.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include "engine/ee/mem/quad_word.h"

namespace sky_oracle {
namespace memory {
extern std::array<uint8_t, 0x400000> bytes;
inline uint32_t Address(uint32_t address, size_t size) {
    address &= 0x1fffffff;
    if (address > bytes.size() || size > bytes.size() - address) {
        std::fprintf(stderr, "sky oracle RAM overrun: %08x + %zu\n", address, size);
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
inline void write_quad(uint32_t address, const QuadWord& value) { write(address & ~15u, value); }
}  // namespace memory
void FUN_001b9f30(CpuContext&, uint32_t);
}  // namespace sky_oracle
