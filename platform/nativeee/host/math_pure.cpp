// platform/nativeee (host side) -- the PURE half of include/platform/math.h and the VU0 bounds functions (PS2 bodies
// src/platform/ps2/{bounds,math}.cpp): the ones whose semantics the decomp states exactly, without a VU0 microprogram.
// Added with the vtable thunks: with their weak stubs (return 0, outputs untouched)
// a zero-height hull box made CastHullDown's step loop never end on the first chunk, so the title was never reached.
//   PointsBox / TriangleBounds / GetBbox  -- VU0 vmini/vmax over the points (GetBbox after a row-vector transform, in
//                                            the asm's vmulax/vmadday/vmaddaz/vmaddw accumulation order)
//   SinCos                                 -- the game's own SinCosPolynomial (include/platform/math.h: the microprogram's
//                                            degree-9 polynomial, bit for bit what the game's CPU path computes)
//   DivideBySquareRoot                     -- a / sqrt(b) (the EE's RSQRT.S; its last-bit rounding is the oracle's to pin)
// The microprogram functions (JointMatrix 0x548/0x668/0x790, ViewDistance 0xCB0, SetRay/StartRayTriangle/FinishRayTriangle
// 0xAB8, ParticleBlockView, Slerp/Euler/Turn) stay with the VU0-math bodies: they need the microcode semantics.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2e_seam.h"
#include "te_host.h"

namespace {

struct V4 {
    float x, y, z, w;
};
struct Box {
    V4 min, max;
};
struct M4 {
    V4 r[4];
};

const V4* Vecs(uint32_t a, uint32_t count) { return reinterpret_cast<const V4*>(te::GuestPointer(a, sizeof(V4) * count)); }
Box* BoxW(uint32_t a) { return reinterpret_cast<Box*>(te::GuestPointer(a, sizeof(Box))); }
const M4* Mat(uint32_t a) { return reinterpret_cast<const M4*>(te::GuestPointer(a, sizeof(M4))); }

// vmini.xyz / vmax.xyz: w keeps the first point's (StartBox loads it into both registers)
void TakeMin(V4& m, const V4& p) { m.x = std::fmin(m.x, p.x); m.y = std::fmin(m.y, p.y); m.z = std::fmin(m.z, p.z); }
void TakeMax(V4& m, const V4& p) { m.x = std::fmax(m.x, p.x); m.y = std::fmax(m.y, p.y); m.z = std::fmax(m.z, p.z); }

V4 Scale(const V4& v, float s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
V4 Add(const V4& a, const V4& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
V4 Transform(const M4& m, const V4& p) {
    V4 acc = Scale(m.r[0], p.x);
    acc = Add(acc, Scale(m.r[1], p.y));
    acc = Add(acc, Scale(m.r[2], p.z));
    return Add(acc, Scale(m.r[3], p.w));
}

// include/platform/math.h CosinePolynomial, verbatim (the microprogram's steps)
float CosinePolynomial(float radians) {
    constexpr float MinusInverseTurn = -0x1.45F30Ep-3f;
    constexpr float Term1 = 0x1.921FB4p+2f;
    constexpr float Term3 = -0x1.4ABBC0p+5f;
    constexpr float Term5 = 0x1.4668AEp+6f;
    constexpr float Term7 = -0x1.324CC2p+6f;
    constexpr float Term9 = 0x1.3DAF6Ep+5f;
    float angle = std::fabs(radians);
    float turns = angle * MinusInverseTurn;
    float whole = -static_cast<float>(static_cast<int32_t>(-turns));
    float fraction = whole - turns;
    float t = std::fabs(fraction - 0.5f) - 0.25f;
    float t2 = t * t;
    float t4 = t2 * t2;
    float t8 = t4 * t4;
    float sum = Term3 * t * t2;
    sum += Term7 * t * t2 * t4;
    sum += Term5 * t * t4;
    sum += t * Term1;
    return sum + Term9 * t * t8;
}

}  // namespace

extern "C" {

void ps2e_vu0_points_box(ps2e_u32 points, ps2e_s32 count, ps2e_u32 box) {
    Box* out = BoxW(box);
    if (!out || count <= 0) return;
    const V4* p = Vecs(points, uint32_t(count));
    if (!p) return;
    V4 lo = p[0], hi = p[0];
    for (int32_t i = 1; i < count; ++i) { TakeMin(lo, p[i]); TakeMax(hi, p[i]); }
    out->min = lo;
    out->max = hi;
}

void ps2e_vu0_triangle_bounds(ps2e_u32 box, ps2e_u32 first, ps2e_u32 second, ps2e_u32 third) {
    Box* out = BoxW(box);
    const V4* a = Vecs(first, 1);
    const V4* b = Vecs(second, 1);
    const V4* c = Vecs(third, 1);
    if (!out || !a || !b || !c) return;
    // "The corners' w stay what vf11 and vf12 had": the box's w components are left as they were
    V4 lo = *a, hi = *a;
    TakeMin(lo, *b); TakeMax(hi, *b);
    TakeMin(lo, *c); TakeMax(hi, *c);
    lo.w = out->min.w;
    hi.w = out->max.w;
    out->min = lo;
    out->max = hi;
}

void ps2e_vu0_get_bbox(ps2e_u32 points, ps2e_s32 count, ps2e_u32 box, ps2e_u32 matrix) {
    Box* out = BoxW(box);
    const M4* m = Mat(matrix);
    if (!out || !m || count <= 0) return;
    const V4* p = Vecs(points, uint32_t(count));
    if (!p) return;
    V4 first = Transform(*m, p[0]);
    V4 lo = first, hi = first;
    for (int32_t i = 1; i < count; ++i) {
        V4 q = Transform(*m, p[i]);
        TakeMin(lo, q);
        TakeMax(hi, q);
    }
    out->min = lo;
    out->max = hi;
    // Diagnosis (TE_DIAG_BBOX=0xADDR[,0xADDR]): every bounds query over a hull blob at one of these guest addresses is printed
    // with its matrix row 3 and the box (the sphere-vs-instance-hull path: GetHullBoundingBox before BoxesOverlap)
    static const char* diag = std::getenv("TE_DIAG_BBOX");
    if (diag && std::strstr(diag, "0x")) {
        char want[16];
        std::snprintf(want, sizeof want, "0x%08X", points);
        char want2[16];
        std::snprintf(want2, sizeof want2, "0x%08x", points);
        if (std::strstr(diag, want) || std::strstr(diag, want2))
            std::fprintf(stderr, "[te.math] get_bbox blob 0x%08X n %d matrix row3 (%.3f %.3f %.3f) -> box (%.3f %.3f %.3f)-(%.3f %.3f %.3f)\n", points, count, m->r[3].x, m->r[3].y, m->r[3].z, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
    }
}

void ps2e_math_sin_cos(ps2e_f32 first, ps2e_f32 second, ps2e_u32 out) {
    float* o = reinterpret_cast<float*>(te::GuestPointer(out, 4 * sizeof(float)));
    if (!o) return;
    constexpr float ProgramHalfPi = 0x1.921FB0p+0f;
    o[0] = CosinePolynomial(first - ProgramHalfPi);
    o[1] = CosinePolynomial(first);
    o[2] = CosinePolynomial(second - ProgramHalfPi);
    o[3] = CosinePolynomial(second);
}

ps2e_f32 ps2e_math_divide_by_square_root(ps2e_f32 value, ps2e_f32 square) {
    // RSQRT.S: value / sqrt(|square|); the EE's FPU has no NaN/Inf -- a zero divisor gives the largest magnitude
    const float s = std::sqrt(std::fabs(square));
    if (s == 0.0f) return std::copysign(0x1.FFFFFEp+127f, value);
    return value / s;
}

}  // extern "C"
