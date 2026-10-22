// lib_vu0_sin_cos.inl -- LibVu0SinCos::SineCosine, the ONE host sin/cos (rules
// in lib_vu0_sin_cos.h). Included from lib_vu0_sin_cos.h only.
#include <cmath>

// The Vita's CPU has no fused multiply-add (newlib's fma is a software routine, the game's hottest maths call): there
// the product and the sum are rounded apart; elsewhere the fused operation the reduction and the series were fitted to
#if defined(__vita__)
#define LIB_VU0_SINCOS_FMA(a, b, c) ((a) * (b) + (c))
#else
#define LIB_VU0_SINCOS_FMA(a, b, c) std::fma(a, b, c)
#endif

inline void LibVu0SinCos::SineCosine(float x, float& sine, float& cosine) {
    double xd = static_cast<double>(x);
    if (std::fabs(xd) >= kLargeArgument) xd = std::fmod(xd, kTwoPi);  // exact; |xd| < 2*pi
    // k = the integer nearest x * 2/pi (floor is rounding-mode independent).
    const double k = std::floor(LIB_VU0_SINCOS_FMA(xd, kTwoOverPi, 0.5));
    double r = LIB_VU0_SINCOS_FMA(-k, kPio2Part1, xd);
    r = LIB_VU0_SINCOS_FMA(-k, kPio2Part2, r);
    r = LIB_VU0_SINCOS_FMA(-k, kPio2Part3, r);
    const double r2 = r * r;
    // sin r = r + r^3 (S1 + r2 (S2 + ... S7)); cos r = (1 - r2/2) + r2^2 (C2 + r2 (C3 + ... C8)).
    double s = kSin[6];
    for (int i = 5; i >= 0; --i) s = LIB_VU0_SINCOS_FMA(s, r2, kSin[i]);
    const double r3 = r * r2;
    s = LIB_VU0_SINCOS_FMA(r3, s, r);
    if (s == 0.0) s = r;  // sin(+-0) keeps the argument's sign ((-0 * S) + -0 is +0 in IEEE)
    double c = kCos[6];
    for (int i = 5; i >= 0; --i) c = LIB_VU0_SINCOS_FMA(c, r2, kCos[i]);
    const double r4 = r2 * r2;
    c = LIB_VU0_SINCOS_FMA(r4, c, LIB_VU0_SINCOS_FMA(-0.5, r2, 1.0));
    // The quadrant: fmod is exact, so k of any magnitude lands in -3..3.
    int q = static_cast<int>(std::fmod(k, 4.0));
    if (q < 0) q += 4;
    double sine_d, cosine_d;
    switch (q) {
        case 0: sine_d = s; cosine_d = c; break;
        case 1: sine_d = c; cosine_d = -s; break;
        case 2: sine_d = -s; cosine_d = -c; break;
        default: sine_d = -c; cosine_d = s; break;
    }
    sine = static_cast<float>(sine_d);
    cosine = static_cast<float>(cosine_d);
}
