// VuFloat -- the non-inlined VU float helpers. Rules in the header.
#include "engine/vu/vu_float.h"

#include <cmath>

alignas(16) const uint32_t VuFloat::kLaneMask[16][4] = {
    {0u, 0u, 0u, 0u},     {0u, 0u, 0u, ~0u},     {0u, 0u, ~0u, 0u},     {0u, 0u, ~0u, ~0u},
    {0u, ~0u, 0u, 0u},    {0u, ~0u, 0u, ~0u},    {0u, ~0u, ~0u, 0u},    {0u, ~0u, ~0u, ~0u},
    {~0u, 0u, 0u, 0u},    {~0u, 0u, 0u, ~0u},    {~0u, 0u, ~0u, 0u},    {~0u, 0u, ~0u, ~0u},
    {~0u, ~0u, 0u, 0u},   {~0u, ~0u, 0u, ~0u},   {~0u, ~0u, ~0u, 0u},   {~0u, ~0u, ~0u, ~0u},
};

void VuFloat::ClampStoreACC(VR_reg& acc) {
    for (int k = 0; k < 4; ++k) acc.I[k] = ClampBits1(acc.I[k]);
}

void VuFloat::WriteMaskClamp(VR_reg& dst, const uint32_t result_bits[4], uint8_t mask) {
    const uint32_t* lm = kLaneMask[mask & 0xF];
    for (int k = 0; k < 4; ++k) {
        if (lm[k]) dst.I[k] = ClampBits1(result_bits[k]);
    }
}

uint16_t VuFloat::PackMacBits(const uint32_t bits[4], uint8_t mask) {
    uint16_t m = 0;
    if (mask & 8) m |= MacFlags1(bits[0], 3);
    if (mask & 4) m |= MacFlags1(bits[1], 2);
    if (mask & 2) m |= MacFlags1(bits[2], 1);
    if (mask & 1) m |= MacFlags1(bits[3], 0);
    return m;
}

void VuFloat::CommitQStatus(VUregs& regs, uint16_t q_status) {
    regs.status_flag = static_cast<uint16_t>((regs.status_flag & 0xFCFu) | (q_status & 0xC30u));
}

void VuFloat::ApplyMinMaxRaw(VR_reg& dest, const VR_reg& fs_raw, const uint32_t ft_raw[4], uint8_t mask,
                             bool protect_vf0, MinMaxFn fn) {
    if (protect_vf0) return;
    uint32_t out[4];
    for (int k = 0; k < 4; ++k) out[k] = fn(fs_raw.I[k], ft_raw[k]);
    if (mask & 8) dest.I[0] = out[0];
    if (mask & 4) dest.I[1] = out[1];
    if (mask & 2) dest.I[2] = out[2];
    if (mask & 1) dest.I[3] = out[3];
}

