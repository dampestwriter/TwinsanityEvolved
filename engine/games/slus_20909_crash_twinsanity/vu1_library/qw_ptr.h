#pragma once
// Quadword accessor over a 16 KB VU1 data-memory working copy: the QW index
// is masked to the 1024 physical quadwords (the VU1 address wrap) and lanes
// are read / written as raw 32-bit words (LQ/SQ move bits, never values).
#include <cstdint>

struct QwPtr {
    uint8_t* mem;

    uint32_t W(uint32_t qw, int lane) const;
    void SetW(uint32_t qw, int lane, uint32_t v);
    float F(uint32_t qw, int lane) const;
};
