// vu_float.inl -- the hot scalar helpers of VuFloat, inlined for the tiers.
// Included from vu_float.h only.
#include <cmath>
#include <cstring>

inline float VuFloat::Bits2F(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline uint32_t VuFloat::F2Bits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

inline uint32_t VuFloat::ClampBits1(uint32_t bits) {
    const uint32_t exp = bits & kExpMask;
    if (exp == kExpMask) return (bits & kSignBit) | kMaxFloatBits;      // NaN / Inf -> +/-MAX
    if (exp == 0 && (bits & kMantMask) != 0) return bits & kSignBit;    // denormal -> +/-0
    return bits;
}

inline float VuFloat::ClampFloat(float v) {
    return Bits2F(ClampBits1(F2Bits(v)));
}

inline uint16_t VuFloat::MacFlags1(uint32_t bits, int field) {
    uint16_t flags = 0;
    const uint32_t exp = (bits >> 23) & 0xFFu;
    if (bits & kSignBit) flags |= static_cast<uint16_t>(1u << (field + 4));   // S from the raw sign, -0 included
    if (exp == 0) {
        flags |= static_cast<uint16_t>(1u << field);                            // Z: true zero and denormal
        if (bits & kMantMask) flags |= static_cast<uint16_t>(1u << (field + 8));  // U: denormal only
    } else if (exp == 0xFFu) {
        flags |= static_cast<uint16_t>(1u << (field + 12));                     // O: Inf / NaN pattern
    }
    return flags;
}

inline uint32_t VuFloat::FpMax(uint32_t a, uint32_t b) {
    const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
    if (sa < 0 && sb < 0) return static_cast<uint32_t>(sa < sb ? sa : sb);
    return static_cast<uint32_t>(sa > sb ? sa : sb);
}

inline uint32_t VuFloat::FpMin(uint32_t a, uint32_t b) {
    const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
    if (sa < 0 && sb < 0) return static_cast<uint32_t>(sa > sb ? sa : sb);
    return static_cast<uint32_t>(sa < sb ? sa : sb);
}

inline uint32_t VuFloat::FtoiSat(float f) {
    const uint32_t ub = F2Bits(f);
    if ((ub & kExpMask) >= 0x4F000000u) return (ub & kSignBit) ? 0x80000000u : 0x7FFFFFFFu;  // |f| >= 2^31, NaN
    return static_cast<uint32_t>(static_cast<int32_t>(f));                                    // truncation toward zero
}

inline uint32_t VuFloat::Itof0(uint32_t lane_bits) {
    return F2Bits(static_cast<float>(static_cast<int32_t>(lane_bits)));
}

inline float VuFloat::VuDiv(float num, float den) {
    num = ClampFloat(num);
    den = ClampFloat(den);
    if (den == 0.0f) {
        const uint32_t sign = (std::signbit(num) != std::signbit(den)) ? kSignBit : 0u;
        return Bits2F(sign | kMaxFloatBits);
    }
    return ClampFloat(num / den);  // under the caller's rounding mode (FE_TOWARDZERO inside the guard)
}

inline uint32_t VuFloat::Ftoi0(uint32_t lane_bits) {
    return FtoiSat(ClampFloat(Bits2F(lane_bits)));
}

inline uint32_t VuFloat::Ftoi4(uint32_t lane_bits) {
    return FtoiSat(Bits2F(lane_bits) * 16.0f);
}

inline uint32_t VuFloat::Ftoi12(uint32_t lane_bits) {
    return FtoiSat(Bits2F(lane_bits) * 4096.0f);
}

inline uint32_t VuFloat::Ftoi15(uint32_t lane_bits) {
    return FtoiSat(Bits2F(lane_bits) * 32768.0f);
}

inline uint32_t VuFloat::ClipNf(uint32_t xb_u, uint32_t yb_u, uint32_t zb_u, uint32_t wb_u) {
    const int32_t wb = (int32_t)wb_u;
    const int32_t value = (wb & 0x7f800000) ? (int32_t)(wb_u & 0x7fffffff) : 0x007fffff;
    uint32_t nf = 0;
    const int32_t xb = (int32_t)xb_u, yb = (int32_t)yb_u, zb = (int32_t)zb_u;
    if ((int32_t)(xb ^ (int32_t)0x00000000) > value) nf |= 0x01;
    if ((int32_t)(xb ^ (int32_t)0x80000000) > value) nf |= 0x02;
    if ((int32_t)(yb ^ (int32_t)0x00000000) > value) nf |= 0x04;
    if ((int32_t)(yb ^ (int32_t)0x80000000) > value) nf |= 0x08;
    if ((int32_t)(zb ^ (int32_t)0x00000000) > value) nf |= 0x10;
    if ((int32_t)(zb ^ (int32_t)0x80000000) > value) nf |= 0x20;
    return nf;
}

#if defined(PS2R_SIMD) && PS2R_SIMD && defined(__aarch64__)
inline uint32x4_t VuFloat::ClampBits4(uint32x4_t bits) {
    const uint32x4_t expm = vdupq_n_u32(kExpMask);
    const uint32x4_t exp = vandq_u32(bits, expm);
    const uint32x4_t sign = vandq_u32(bits, vdupq_n_u32(kSignBit));
    const uint32x4_t is_naninf = vceqq_u32(exp, expm);
    const uint32x4_t is_denorm = vandq_u32(vceqq_u32(exp, vdupq_n_u32(0)), vtstq_u32(bits, vdupq_n_u32(kMantMask)));
    uint32x4_t r = vbslq_u32(is_naninf, vorrq_u32(sign, vdupq_n_u32(kMaxFloatBits)), bits);
    r = vbslq_u32(is_denorm, sign, r);
    return r;
}

inline uint16_t VuFloat::MacFlags4(uint32x4_t bits, uint8_t mask) {
    const uint32x4_t expm = vdupq_n_u32(kExpMask);
    const uint32x4_t exp = vandq_u32(bits, expm);
    const uint32x4_t z = vceqq_u32(exp, vdupq_n_u32(0));
    const uint32x4_t o = vceqq_u32(exp, expm);
    const uint32x4_t u = vandq_u32(z, vtstq_u32(bits, vdupq_n_u32(kMantMask)));
    const uint32x4_t s = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(bits), 31));
    const uint32x4_t zb = {1u << 3, 1u << 2, 1u << 1, 1u << 0};
    const uint32x4_t sb = {1u << 7, 1u << 6, 1u << 5, 1u << 4};
    const uint32x4_t ub = {1u << 11, 1u << 10, 1u << 9, 1u << 8};
    const uint32x4_t ob = {1u << 15, 1u << 14, 1u << 13, 1u << 12};
    uint32x4_t acc = vorrq_u32(vorrq_u32(vandq_u32(z, zb), vandq_u32(s, sb)),
                               vorrq_u32(vandq_u32(u, ub), vandq_u32(o, ob)));
    acc = vandq_u32(acc, vld1q_u32(kLaneMask[mask & 0xF]));
    return static_cast<uint16_t>(vaddvq_u32(acc));  // lane bits are disjoint: the sum is the OR
}
#endif

// The float product is exact in double and the sum rounds once there; rounding that double to float again is the fused
// result unless it lies exactly halfway between two floats (round to nearest would then round twice) or outside the normal
// float range (a subnormal's coarser grid): those rare sums go to std::fmaf. Under the VU's round toward zero the double
// path is always exact.
inline float VuFloat::Fma(float a, float b, float c) {
#if defined(__FMA__) || defined(__ARM_FEATURE_FMA)
    return __builtin_fmaf(a, b, c);
#elif defined(__vita__)
    // The Vita's Cortex-A9 has no fused multiply-add: the exact route below is two double-precision operations and a
    // check per multiply-add, the largest single cost of its geometry. The Vita takes the product and the sum rounded
    // apart instead (within a unit in the last place of the fused result; the projection keeps 1/16 pixel).
    return a * b + c;
#else
    const double r = double(a) * double(b) + double(c);
    uint64_t bits;
    std::memcpy(&bits, &r, sizeof(bits));
    const uint64_t mag = bits & 0x7FFFFFFFFFFFFFFFull;
    const bool normal = (mag >= 0x3810000000000000ull && mag <= 0x47EFFFFFE0000000ull) || mag == 0;   // 2^-126 .. FLT_MAX
    if (normal && (bits & 0x1FFFFFFFull) != 0x10000000ull) return static_cast<float>(r);
    return std::fmaf(a, b, c);
#endif
}
