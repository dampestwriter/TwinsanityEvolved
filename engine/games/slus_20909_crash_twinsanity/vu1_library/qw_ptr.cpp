// QwPtr -- masked quadword lane access (rule in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/qw_ptr.h"

#include <cstring>

uint32_t QwPtr::W(uint32_t qw, int lane) const {
    uint32_t v;
    std::memcpy(&v, mem + ((qw & 0x3FF) * 16) + lane * 4, 4);
    return v;
}

void QwPtr::SetW(uint32_t qw, int lane, uint32_t v) { std::memcpy(mem + ((qw & 0x3FF) * 16) + lane * 4, &v, 4); }

float QwPtr::F(uint32_t qw, int lane) const {
    float f;
    const uint32_t b = W(qw, lane);
    std::memcpy(&f, &b, 4);
    return f;
}
