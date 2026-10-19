#include "games/slus_20909_crash_twinsanity/hle/math/trig_math.h"

#include <cmath>
#include <cstring>

#include "engine/ipu/hle/guest_memory.h"

namespace title {

TrigMath::Vector TrigMath::EvaluateRotation(int32_t angle, Axis axis) {
    Vector result{0, 0, 0, 1};
    if (angle != 0) {
        volatile float half = AngleRadians(angle) * 0.5f;
        const auto values = EvaluateSinCos(half, 0);
        result[static_cast<size_t>(axis)] = values[0];
        result[3] = values[1];
    }
    return result;
}

TrigMath::Vector TrigMath::EvaluateRotationPair(int32_t first, int32_t second, AxisPair axes) {
    const Axis first_axis = axes == AxisPair::YZ ? Axis::Yaw : Axis::Pitch;
    const Axis second_axis = axes == AxisPair::XY ? Axis::Yaw : Axis::Roll;
    if (first == 0) return EvaluateRotation(second, second_axis);
    if (second == 0) return EvaluateRotation(first, first_axis);
    volatile float first_half = AngleRadians(first) * 0.5f;
    volatile float second_half = AngleRadians(second) * 0.5f;
    return ComposeAngles(EvaluateSinCos(first_half, second_half), axes);
}

TrigMath::Vector TrigMath::EvaluateRotationXYZ(int32_t x, int32_t y, int32_t z) {
    if (x == 0) return EvaluateRotationPair(y, z, AxisPair::YZ);
    if (y == 0) return EvaluateRotationPair(x, z, AxisPair::XZ);
    if (z == 0) return EvaluateRotationPair(x, y, AxisPair::XY);
    const auto xz = EvaluateSinCos(Product(AngleRadians(x), 0.5f), Product(AngleRadians(z), 0.5f));
    const auto yy = EvaluateSinCos(Product(AngleRadians(y), 0.5f), 0);
    const float cc = Product(xz[1], xz[3]), ss = Product(xz[0], xz[2]);
    const float sc = Product(xz[0], xz[3]), cs = Product(xz[1], xz[2]);
    return {Difference(Product(sc, yy[1]), Product(cs, yy[0])),
        Sum(Product(ss, yy[1]), Product(cc, yy[0])),
        Difference(Product(cs, yy[1]), Product(sc, yy[0])),
        Sum(Product(cc, yy[1]), Product(ss, yy[0]))};
}

TrigMath::Vector TrigMath::EvaluateTurnToward(float share, const Vector& from, const Vector& to) {
    const float cosine = Sum(Sum(Product(from[0], to[0]), Product(from[1], to[1])), Product(from[2], to[2]));
    const float angle = ArcCosine(cosine);
    const float sine = EvaluateSinCos(angle, 0)[0];
    if (sine == 0) return to;
    const float inverse = NearestQuotient(1, sine);
    const auto weights = EvaluateSinCos(Product(angle, Difference(1, share)), Product(angle, share));
    Vector result = from;
    for (size_t lane = 0; lane < 3; ++lane)
        result[lane] = Sum(Product(Product(from[lane], inverse), weights[0]),
                           Product(Product(to[lane], inverse), weights[2]));
    return result;
}

TrigMath::Vector TrigMath::EvaluateTurnBySineCosine(
    float sine, float cosine, const Vector& vector, const Vector& axis, bool unit_axis) {
    float x = axis[0], y = axis[1], z = axis[2];
    float scale = 1;
    if (!unit_axis) {
        const float squared = Sum(Sum(Product(x, x), Product(y, y)), Product(z, z));
        scale = FromBits(0x312bcc76) < squared ? NearestQuotient(1, NearestRoot(squared)) : 0;
        x = Product(x, scale);
        y = Product(y, scale);
        z = Product(z, scale);
    }
    if (!(0 < scale)) return vector;
    const float vx = vector[0], vy = vector[1], vz = vector[2];
    const float along = Product(Sum(Sum(Product(vx, x), Product(vy, y)), Product(vz, z)),
                                Difference(1, cosine));
    const float cross_x = Product(Difference(Product(y, vz), Product(z, vy)), sine);
    const float cross_y = Product(Difference(Product(z, vx), Product(x, vz)), sine);
    const float cross_z = Product(Difference(Product(x, vy), Product(y, vx)), sine);
    return {Sum(Sum(cross_x, Product(x, along)), Product(vx, cosine)),
            Sum(Sum(cross_y, Product(y, along)), Product(vy, cosine)),
            Sum(Sum(cross_z, Product(z, along)), Product(vz, cosine)), vector[3]};
}

TrigMath::Vector TrigMath::EvaluateTurnAboutAxis(
    int32_t angle, const Vector& vector, const Vector& axis, bool unit_axis) {
    if (angle == 0) return vector;
    const auto values = EvaluateSinCos(AngleRadians(angle), 0);
    return EvaluateTurnBySineCosine(values[0], values[1], vector, axis, unit_axis);
}

TrigMath::Vector TrigMath::EvaluateRotationFromAxisSine(
    float sine, float cosine, const Vector& axis, bool unit_axis) {
    float scale = sine;
    if (!unit_axis) {
        const float squared = Sum(Sum(Product(axis[0], axis[0]), Product(axis[1], axis[1])),
                                  Product(axis[2], axis[2]));
        const float inverse = FromBits(0x312bcc76) < squared ? NearestQuotient(1, NearestRoot(squared)) : 0;
        scale = Product(sine, inverse);
    }
    return {Product(scale, axis[0]), Product(scale, axis[1]), Product(scale, axis[2]), cosine};
}

TrigMath::Vector TrigMath::EvaluateRotationAboutAxis(int32_t angle, const Vector& axis, bool unit_axis) {
    volatile float converted = static_cast<float>(angle);
    const int32_t half = static_cast<int32_t>(Product(converted, 0.5f));
    const auto values = EvaluateSinCos(AngleRadians(half), 0);
    return EvaluateRotationFromAxisSine(values[0], values[1], axis, unit_axis);
}

float TrigMath::EvaluateInverseLength4(float fallback, float epsilon, const Vector& vector) {
    const float squared = Sum(Sum(Sum(Product(vector[0], vector[0]), Product(vector[1], vector[1])),
                                  Product(vector[2], vector[2])), Product(vector[3], vector[3]));
    return Product(epsilon, epsilon) < squared ? NearestQuotient(1, NearestRoot(squared)) : fallback;
}

TrigMath::Vector TrigMath::EvaluateSlerp(float share, const Vector& from, const Vector& to) {
    float cosine = Sum(Sum(Sum(Product(from[0], to[0]), Product(from[1], to[1])),
                           Product(from[2], to[2])), Product(from[3], to[3]));
    const bool opposite = cosine < 0;
    if (opposite) cosine = -cosine;
    Vector result;
    if (FromBits(0x3d4ccccd) < Difference(1, cosine)) {
        const float angle = ArcCosine(cosine);
        const float sine = EvaluateSinCos(angle, 0)[0];
        const float scaled = Product(angle, share);
        const auto weights = EvaluateSinCos(scaled, Difference(scaled, angle));
        const float inverse = NearestQuotient(1, sine);
        const float to_weight = Product(weights[0], inverse);
        float from_weight = Product(weights[2], inverse);
        if (!opposite) from_weight = -from_weight;
        for (unsigned lane = 0; lane < 4; ++lane)
            result[lane] = Sum(Product(from[lane], from_weight), Product(to[lane], to_weight));
        return result;
    }
    for (unsigned lane = 0; lane < 4; ++lane) result[lane] = opposite ? -from[lane] : from[lane];
    if (!(0 < share)) return result;
    if (!(share < 1)) return to;
    for (unsigned lane = 0; lane < 4; ++lane)
        result[lane] = Sum(Product(Difference(to[lane], result[lane]), share), result[lane]);
    const float inverse = EvaluateInverseLength4(0, FromBits(0x2edbe6ff), result);
    for (auto& lane : result) lane = Product(lane, inverse);
    return result;
}

int32_t TrigMath::EvaluateAngleFrom(float value, uint32_t unit) {
    const uint32_t scale = unit == 1 ? 0x43360b61 : unit == 2 ? 0x47800000 : 0x4622f983;
    return TruncatedAngle(Product(value, FromBits(scale)));
}

int32_t TrigMath::EvaluateAngleOfPoint(float y, float x) {
    const float beyond = Sum(NearestRoot(Sum(Product(x, x), Product(y, y))), x);
    if (!(0 < beyond)) return 32768;
    const float tangent = NearestQuotient(y, beyond);
    float half;
    if (Product(tangent, tangent) < 1) half = PointAngleEstimate(tangent);
    else half = Difference(FromBits(tangent > 0 ? 0x3fc90fdb : 0xbfc90fdb),
                           PointAngleEstimate(NearestQuotient(1, tangent)));
    const int32_t half_angle = EvaluateAngleFrom(half, 0);
    const float radians = Product(static_cast<float>(half_angle), FromBits(0x38c90fdb));
    return EvaluateAngleFrom(Sum(radians, radians), 0);
}

void TrigMath::Rotation(CpuContext& context, Axis axis) {
    calls_[3 + static_cast<size_t>(axis)].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    r[5].SD[0] = static_cast<int32_t>(memory_.Read32(r[5].UL[0]));
    MoveStack(context, uint32_t(-16));
    const uint32_t output = r[4].UL[0];
    if (r[5].SL[0] == 0) {
        r[1].SD[0] = 0x3f800000;
        f[0].UL = 0x3f800000;
        // Source zero fast path does not call trig or modify VU scratch.
        memory_.Write32(output + 8, r[0].UL[0]);
        memory_.Write32(output + 12, f[0].UL);
        memory_.Write32(output + 4, r[0].UL[0]);
        memory_.Write32(output, r[0].UL[0]);
    } else {
        f[1].UL = 0x38c90fdb;
        r[1].SD[0] = 0x3f000000;
        f[2].UL = 0x3f000000;
        volatile float half = AngleRadians(r[5].SL[0]) * 0.5f;
        f[0].f = half;
        memory_.Write32(r[29].UL[0], f[0].UL);
        EvaluateScratch(context);
        f[0].UL = memory_.Read32(r[29].UL[0] + 4);
        f[1].UL = memory_.Read32(r[29].UL[0]);
        f[2].UL = r[0].UL[0];
        memory_.Write32(output + 12, f[0].UL);
        const uint32_t component = static_cast<uint32_t>(axis) * 4;
        memory_.Write32(output + (axis == Axis::Pitch ? 4 : 0), f[2].UL);
        memory_.Write32(output + component, f[1].UL);
        memory_.Write32(output + (axis == Axis::Roll ? 4 : 8), f[2].UL);
    }
    MoveStack(context, 16);
}

void TrigMath::RotationFromPitch(CpuContext& context) { Rotation(context, Axis::Pitch); }

void TrigMath::RotationFromYaw(CpuContext& context) { Rotation(context, Axis::Yaw); }

void TrigMath::RotationFromRoll(CpuContext& context) { Rotation(context, Axis::Roll); }

void TrigMath::RotationPair(CpuContext& context, AxisPair axes) {
    calls_[6 + static_cast<size_t>(axes)].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    r[5].SD[0] = static_cast<int32_t>(memory_.Read32(r[5].UL[0]));
    MoveStack(context, uint32_t(-48));
    r[6].SD[0] = static_cast<int32_t>(memory_.Read32(r[6].UL[0]));
    const uint32_t stack = r[29].UL[0], output = r[4].UL[0];
    if (r[5].SL[0] == 0) memory_.Write32(stack, r[6].UL[0]);
    if (r[5].SL[0] == 0 && r[6].SL[0] == 0) {
        r[1].SD[0] = 0x3f800000;
        f[0].UL = 0x3f800000;
        memory_.Write32(output + 8, r[0].UL[0]);
        memory_.Write32(output + 12, f[0].UL);
        memory_.Write32(output + 4, r[0].UL[0]);
        memory_.Write32(output, r[0].UL[0]);
    } else if (r[5].SL[0] == 0 || r[6].SL[0] == 0) {
        const bool first = r[5].SL[0] != 0;
        const uint32_t offset = first ? 32 : 16;
        const uint32_t component = first ? (axes == AxisPair::YZ ? 4 : 0) : (axes == AxisPair::XY ? 4 : 8);
        const int32_t angle = first ? r[5].SL[0] : r[6].SL[0];
        r[1].SD[0] = 0x3f000000;
        f[1].UL = 0x38c90fdb;
        f[2].UL = 0x3f000000;
        if (first) memory_.Write32(stack, r[5].UL[0]);
        volatile float half = AngleRadians(angle) * 0.5f;
        f[0].f = half;
        memory_.Write32(stack + offset, f[0].UL);
        EvaluateScratch(context, offset);
        f[0].UL = memory_.Read32(stack + offset + 4);
        f[1].UL = memory_.Read32(stack + offset);
        // XY/XZ's first-only case stores x before w; the other paths store w
        // first. This matters when output aliases the stack or its own inputs.
        if (component == 0) {
            memory_.Write32(output, f[1].UL);
            memory_.Write32(output + 12, f[0].UL);
            memory_.Write32(output + 4, r[0].UL[0]);
            memory_.Write32(output + 8, r[0].UL[0]);
        } else {
            memory_.Write32(output + 12, f[0].UL);
            memory_.Write32(output + component, f[1].UL);
            memory_.Write32(output, r[0].UL[0]);
            memory_.Write32(output + (component == 4 ? 8 : 4), r[0].UL[0]);
        }
    } else {
        r[1].SD[0] = 0x3f000000;
        f[2].UL = 0x38c90fdb;
        f[3].UL = 0x3f000000;
        volatile float first_half = AngleRadians(r[5].SL[0]) * 0.5f;
        volatile float second_half = AngleRadians(r[6].SL[0]) * 0.5f;
        f[1].f = first_half;
        f[0].f = second_half;
        memory_.Write32(stack, f[1].UL);
        memory_.Write32(stack + 8, f[0].UL);
        EvaluateScratch(context);
        Vector values;
        for (uint32_t lane = 0; lane < 4; ++lane) values[lane] = FromBits(memory_.Read32(stack + lane * 4));
        const auto result = ComposeAngles(values, axes);
        if (axes == AxisPair::XY) {
            f[0].f = result[1];
            f[1].f = result[0];
            f[2].f = result[2];
            f[3].f = result[3];
            f[4].f = values[3];
            memory_.Write32(output + 8, f[2].UL);
            memory_.Write32(output + 4, f[0].UL);
            memory_.Write32(output, f[1].UL);
            memory_.Write32(output + 12, f[3].UL);
        } else if (axes == AxisPair::XZ) {
            f[0].f = result[1];
            f[1].f = values[2];
            f[2].f = result[3];
            f[3].f = values[3];
            f[4].f = result[0];
            f[5].f = result[2];
            memory_.Write32(output, f[4].UL);
            memory_.Write32(output + 8, f[5].UL);
            memory_.Write32(output + 4, f[0].UL);
            memory_.Write32(output + 12, f[2].UL);
        } else {
            f[0].f = result[1];
            f[1].f = values[2];
            f[2].f = result[3];
            f[3].f = result[0];
            f[4].f = values[3];
            f[5].f = result[2];
            memory_.Write32(output + 8, f[5].UL);
            memory_.Write32(output + 4, f[0].UL);
            memory_.Write32(output, f[3].UL);
            memory_.Write32(output + 12, f[2].UL);
        }
    }
    MoveStack(context, 48);
}

void TrigMath::GetRotationXY(CpuContext& context) { RotationPair(context, AxisPair::XY); }

void TrigMath::GetRotationXZ(CpuContext& context) { RotationPair(context, AxisPair::XZ); }

void TrigMath::GetRotationYZ(CpuContext& context) { RotationPair(context, AxisPair::YZ); }

void TrigMath::GetRotationXYZ(CpuContext& context) {
    calls_[12].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-80));
    const uint32_t stack = r[29].UL[0];
    memory_.Write32(stack + 64, static_cast<uint32_t>(saved_return));
    memory_.Write32(stack + 68, static_cast<uint32_t>(saved_return >> 32));
    r[5].SD[0] = static_cast<int32_t>(memory_.Read32(r[5].UL[0]));
    r[2].SD[0] = static_cast<int32_t>(memory_.Read32(r[6].UL[0]));
    if (r[5].SD[0] == 0) {
        r[5].UD[0] = r[29].UD[0] + r[0].UD[0];
        r[3].SD[0] = static_cast<int32_t>(memory_.Read32(r[7].UL[0]));
        r[6].SD[0] = static_cast<int32_t>(stack + 16);
        memory_.Write32(stack, r[2].UL[0]);
        memory_.Write32(stack + 16, r[3].UL[0]);
        r[31].UL[0] = 0x1867ac;
        GetRotationYZ(context);
    } else if (r[2].SD[0] == r[0].SD[0]) {
        r[2].SD[0] = static_cast<int32_t>(memory_.Read32(r[7].UL[0]));
        r[6].SD[0] = static_cast<int32_t>(stack + 32);
        memory_.Write32(stack, r[5].UL[0]);
        r[5].UD[0] = r[29].UD[0] + r[0].UD[0];
        memory_.Write32(stack + 32, r[2].UL[0]);
        r[31].UL[0] = 0x1867d4;
        GetRotationXZ(context);
    } else {
        r[3].SD[0] = static_cast<int32_t>(memory_.Read32(r[7].UL[0]));
        r[6].SD[0] = static_cast<int32_t>(stack + 48);
        if (r[3].SD[0] == 0) {
            memory_.Write32(stack, r[5].UL[0]);
            memory_.Write32(stack + 48, r[2].UL[0]);
            r[5].UD[0] = r[29].UD[0] + r[0].UD[0];
            r[31].UL[0] = 0x1867f4;
            GetRotationXY(context);
        } else {
            r[1].SD[0] = 0x3f000000;
            f[0].UL = 0x38c90fdb;
            f[2].UL = 0x3f000000;
            f[3].f = Product(AngleRadians(r[5].SL[0]), 0.5f);
            f[1].f = Product(AngleRadians(r[3].SL[0]), 0.5f);
            f[4].f = Product(AngleRadians(r[2].SL[0]), 0.5f);
            memory_.Write32(stack, f[3].UL);
            memory_.Write32(stack + 8, f[1].UL);
            EvaluateScratch(context);
            f[3].UL = memory_.Read32(stack);
            f[0].UL = memory_.Read32(stack + 4);
            f[1].UL = memory_.Read32(stack + 12);
            f[2].UL = memory_.Read32(stack + 8);
            memory_.Write32(stack, f[4].UL);
            EvaluateScratch(context, 0, false);
            f[6].f = Product(f[0].f, f[2].f);
            f[5].f = Product(f[3].f, f[1].f);
            f[0].f = Product(f[0].f, f[1].f);
            f[3].f = Product(f[3].f, f[2].f);
            // The source publishes the second scratch after the four x/z
            // products. Scalar reads remain unaligned while quad stores align.
            for (uint32_t i = 0; i < 4; ++i)
                memory_.Write32((stack & ~15u) + i * 4, context.vuRegs.VF[31].I[i]);
            f[1].UL = memory_.Read32(stack + 4);
            f[2].UL = memory_.Read32(stack);
            f[7].f = Product(f[6].f, f[1].f);
            f[9].f = Product(f[5].f, f[2].f);
            f[4].f = Product(f[3].f, f[1].f);
            f[8].f = Product(f[0].f, f[2].f);
            f[0].f = Product(f[0].f, f[1].f);
            f[3].f = Product(f[3].f, f[2].f);
            f[5].f = Product(f[5].f, f[1].f);
            f[6].f = Product(f[6].f, f[2].f);
            f[0].f = Sum(f[0].f, f[3].f);
            f[4].f = Sum(f[4].f, f[8].f);
            f[5].f = Difference(f[5].f, f[6].f);
            f[7].f = Difference(f[7].f, f[9].f);
            memory_.Write32(r[4].UL[0] + 12, f[0].UL);
            memory_.Write32(r[4].UL[0] + 4, f[4].UL);
            memory_.Write32(r[4].UL[0], f[5].UL);
            memory_.Write32(r[4].UL[0] + 8, f[7].UL);
        }
    }
    r[31].UD[0] = saved_return;
    MoveStack(context, 80);
}

void TrigMath::TurnToward(CpuContext& context) {
    calls_[18].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    auto& flags = context.fpuRegs.fprc[31];
    const auto compare = [&](bool condition) {
        flags = (flags & ~0x800000u) | (condition ? 0x800000u : 0);
        return condition;
    };
    f[2].UL = memory_.Read32(r[6].UL[0]);
    f[3].UL = memory_.Read32(r[5].UL[0]);
    f[1].UL = memory_.Read32(r[5].UL[0] + 4);
    f[4].UL = memory_.Read32(r[6].UL[0] + 4);
    f[3].f = Product(f[3].f, f[2].f);
    f[0].UL = memory_.Read32(r[5].UL[0] + 8);
    f[1].f = Product(f[1].f, f[4].f);
    f[2].UL = memory_.Read32(r[6].UL[0] + 8);
    r[1].SD[0] = 0x3f800000;
    f[8].UL = 0x3f800000;
    f[0].f = Product(f[0].f, f[2].f);
    f[3].f = Sum(f[3].f, f[1].f);
    f[7].UL = r[0].UL[0];
    f[3].f = Sum(f[3].f, f[0].f);
    f[6].f = std::fabs(f[3].f);
    f[0].f = Difference(f[8].f, f[6].f);
    const bool below_zero = compare(f[0].f < f[7].f);
    MoveStack(context, uint32_t(-48));
    if (below_zero) f[0].UL = f[7].UL;
    f[5].UL = 0xbc996e30;
    const bool negative = compare(f[3].f < f[7].f);
    f[1].UL = 0x3d981627;
    f[3].f = NearestRoot(f[0].f);
    f[5].f = Product(f[6].f, f[5].f);
    f[0].UL = 0x3e593484;
    r[1].SD[0] = 0x3fc90da4;
    f[2].UL = 0x3fc90da4;
    f[5].f = Sum(f[5].f, f[1].f);
    f[5].f = Product(f[5].f, f[6].f);
    f[5].f = Difference(f[5].f, f[0].f);
    f[5].f = Product(f[5].f, f[6].f);
    f[5].f = Sum(f[5].f, f[2].f);
    f[5].f = Product(f[5].f, f[3].f);
    if (negative) {
        r[1].SD[0] = 0x40490fdb;
        f[0].UL = 0x40490fdb;
        f[5].f = Difference(f[0].f, f[5].f);
    }
    const uint32_t stack = r[29].UL[0];
    memory_.Write32(stack, f[5].UL);
    EvaluateScratch(context);
    f[4].UL = memory_.Read32(stack);
    if (compare(f[4].f == f[7].f)) {
        for (uint32_t i = 0; i < 4; ++i)
            r[2].UL[i] = memory_.Read32((r[6].UL[0] & ~15u) + i * 4);
        for (uint32_t i = 0; i < 4; ++i)
            memory_.Write32((r[4].UL[0] & ~15u) + i * 4, r[2].UL[i]);
    } else {
        f[0].f = Difference(f[8].f, f[12].f);
        f[1].f = Product(f[5].f, f[12].f);
        f[4].f = NearestQuotient(f[8].f, f[4].f);
        f[0].f = Product(f[5].f, f[0].f);
        memory_.Write32(stack + 8, f[1].UL);
        memory_.Write32(stack, f[0].UL);
        EvaluateScratch(context, 0, false);
        FinishTurnToward(context);
    }
    MoveStack(context, 48);
}

void TrigMath::FinishTurnToward(CpuContext& context) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t stack = r[29].UL[0], output = r[4].UL[0];
    // The NTSC source stages both complete vectors before any output writes.
    // Raw w follows from, including payloads; scalar reads remain unaligned.
    for (uint32_t i = 0; i < 4; ++i)
        r[2].UL[i] = memory_.Read32((r[5].UL[0] & ~15u) + i * 4);
    for (uint32_t i = 0; i < 4; ++i)
        r[3].UL[i] = memory_.Read32((r[6].UL[0] & ~15u) + i * 4);
    for (uint32_t i = 0; i < 4; ++i) memory_.Write32((stack & ~15u) + i * 4, r[2].UL[i]);
    for (uint32_t i = 0; i < 4; ++i) memory_.Write32(((stack + 16) & ~15u) + i * 4, r[3].UL[i]);
    f[2].UL = memory_.Read32(stack);
    f[5].UL = memory_.Read32(stack + 4);
    f[6].UL = memory_.Read32(stack + 8);
    f[2].f = Product(f[2].f, f[4].f);
    f[3].UL = memory_.Read32(stack + 16);
    f[5].f = Product(f[5].f, f[4].f);
    f[1].UL = memory_.Read32(stack + 20);
    f[6].f = Product(f[6].f, f[4].f);
    f[0].UL = memory_.Read32(stack + 24);
    f[3].f = Product(f[3].f, f[4].f);
    f[1].f = Product(f[1].f, f[4].f);
    memory_.Write32(stack, f[2].UL);
    f[0].f = Product(f[0].f, f[4].f);
    memory_.Write32(stack + 4, f[5].UL);
    memory_.Write32(stack + 8, f[6].UL);
    memory_.Write32(stack + 16, f[3].UL);
    memory_.Write32(stack + 20, f[1].UL);
    memory_.Write32(stack + 24, f[0].UL);
    for (uint32_t i = 0; i < 4; ++i)
        memory_.Write32(((stack + 32) & ~15u) + i * 4, context.vuRegs.VF[31].I[i]);
    f[0].UL = memory_.Read32(stack + 32);
    f[1].UL = memory_.Read32(stack);
    f[2].UL = memory_.Read32(stack + 4);
    f[3].UL = memory_.Read32(stack + 8);
    f[1].f = Product(f[1].f, f[0].f);
    f[2].f = Product(f[2].f, f[0].f);
    f[5].UL = memory_.Read32(stack + 40);
    f[3].f = Product(f[3].f, f[0].f);
    f[6].UL = memory_.Read32(stack + 16);
    memory_.Write32(stack, f[1].UL);
    memory_.Write32(stack + 4, f[2].UL);
    f[6].f = Product(f[6].f, f[5].f);
    memory_.Write32(stack + 8, f[3].UL);
    f[4].UL = memory_.Read32(stack + 20);
    for (uint32_t i = 0; i < 4; ++i) r[2].UL[i] = memory_.Read32((stack & ~15u) + i * 4);
    f[3].UL = memory_.Read32(stack + 24);
    f[4].f = Product(f[4].f, f[5].f);
    for (uint32_t i = 0; i < 4; ++i) memory_.Write32((output & ~15u) + i * 4, r[2].UL[i]);
    memory_.Write32(stack + 16, f[6].UL);
    f[3].f = Product(f[3].f, f[5].f);
    f[0].UL = memory_.Read32(output);
    f[1].UL = memory_.Read32(output + 4);
    f[2].UL = memory_.Read32(output + 8);
    f[0].f = Sum(f[0].f, f[6].f);
    f[1].f = Sum(f[1].f, f[4].f);
    memory_.Write32(stack + 20, f[4].UL);
    f[2].f = Sum(f[2].f, f[3].f);
    memory_.Write32(stack + 24, f[3].UL);
    memory_.Write32(output, f[0].UL);
    memory_.Write32(output + 4, f[1].UL);
    memory_.Write32(output + 8, f[2].UL);
}

void TrigMath::TurnBySineCosine(CpuContext& context) {
    calls_[19].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    auto& flags = context.fpuRegs.fprc[31];
    const auto compare = [&](bool condition) {
        flags = (flags & ~0x800000u) | (condition ? 0x800000u : 0);
        return condition;
    };
    f[11].UL = memory_.Read32(r[5].UL[0] + 8);
    f[14].UL = memory_.Read32(r[5].UL[0]);
    r[1].SD[0] = 0x3f800000;
    f[4].UL = 0x3f800000;
    f[15].UL = memory_.Read32(r[5].UL[0] + 4);
    if (r[6].SD[0] == 0) {
        f[0].f = Product(f[14].f, f[14].f);
        r[1].SD[0] = 0x312bcc76;
        f[3].UL = 0x312bcc76;
        f[1].f = Product(f[15].f, f[15].f);
        f[2].f = Product(f[11].f, f[11].f);
        f[0].f = Sum(f[0].f, f[1].f);
        f[0].f = Sum(f[0].f, f[2].f);
        if (compare(f[3].f < f[0].f)) {
            f[0].f = NearestRoot(f[0].f);
            f[0].f = NearestQuotient(f[4].f, f[0].f);
        } else f[0].UL = r[0].UL[0];
        f[4].UL = f[0].UL;
        f[14].f = Product(f[14].f, f[4].f);
        f[15].f = Product(f[15].f, f[4].f);
        f[11].f = Product(f[11].f, f[4].f);
    }
    f[0].UL = r[0].UL[0];
    if (!compare(f[0].f < f[4].f)) return;
    // Source Rodrigues terms stay separately rounded. Load the vector and
    // axis before the ordered z/x/y stores so partial overlap remains valid.
    f[7].UL = memory_.Read32(r[4].UL[0]);
    f[8].UL = memory_.Read32(r[4].UL[0] + 4);
    f[1].f = Product(f[7].f, f[14].f);
    f[6].UL = memory_.Read32(r[4].UL[0] + 8);
    f[0].f = Product(f[8].f, f[15].f);
    r[1].SD[0] = 0x3f800000;
    f[4].UL = 0x3f800000;
    f[5].f = Product(f[6].f, f[11].f);
    f[4].f = Difference(f[4].f, f[13].f);
    f[1].f = Sum(f[1].f, f[0].f);
    f[9].f = Product(f[14].f, f[6].f);
    f[3].f = Product(f[15].f, f[6].f);
    f[1].f = Sum(f[1].f, f[5].f);
    f[10].f = Product(f[11].f, f[8].f);
    f[2].f = Product(f[11].f, f[7].f);
    f[0].f = Product(f[14].f, f[8].f);
    f[5].f = Product(f[15].f, f[7].f);
    f[1].f = Product(f[1].f, f[4].f);
    f[2].f = Difference(f[2].f, f[9].f);
    f[3].f = Difference(f[3].f, f[10].f);
    f[0].f = Difference(f[0].f, f[5].f);
    f[9].f = Product(f[11].f, f[1].f);
    f[4].f = Product(f[14].f, f[1].f);
    f[3].f = Product(f[3].f, f[12].f);
    f[0].f = Product(f[0].f, f[12].f);
    f[2].f = Product(f[2].f, f[12].f);
    f[1].f = Product(f[15].f, f[1].f);
    f[0].f = Sum(f[0].f, f[9].f);
    f[3].f = Sum(f[3].f, f[4].f);
    f[2].f = Sum(f[2].f, f[1].f);
    f[6].f = Product(f[6].f, f[13].f);
    f[7].f = Product(f[7].f, f[13].f);
    f[8].f = Product(f[8].f, f[13].f);
    f[0].f = Sum(f[0].f, f[6].f);
    f[3].f = Sum(f[3].f, f[7].f);
    f[2].f = Sum(f[2].f, f[8].f);
    memory_.Write32(r[4].UL[0] + 8, f[0].UL);
    memory_.Write32(r[4].UL[0], f[3].UL);
    memory_.Write32(r[4].UL[0] + 4, f[2].UL);
}

void TrigMath::TurnAboutAxis(CpuContext& context) {
    calls_[20].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-32));
    const uint32_t stack = r[29].UL[0];
    memory_.Write32(stack + 16, static_cast<uint32_t>(saved_return));
    memory_.Write32(stack + 20, static_cast<uint32_t>(saved_return >> 32));
    r[6].SD[0] = static_cast<int32_t>(memory_.Read32(r[6].UL[0]));
    if (r[6].SD[0] != r[0].SD[0]) {
        f[0].f = AngleRadians(r[6].SL[0]);
        r[1].SD[0] = 0x38c90fdb;
        f[1].UL = 0x38c90fdb;
        memory_.Write32(stack, f[0].UL);
        EvaluateScratch(context);
        f[13].UL = memory_.Read32(stack + 4);
        r[6].SD[0] = r[7].SD[0] + r[0].SD[0];
        f[12].UL = memory_.Read32(stack);
        r[31].UL[0] = 0x18de60;
        TurnBySineCosine(context);
    }
    r[31].UD[0] = saved_return;
    MoveStack(context, 32);
}

void TrigMath::RotationFromAxisSine(CpuContext& context) {
    calls_[21].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    f[2].UL = f[12].UL;
    if (r[6].SD[0] == 0) {
        f[1].UL = memory_.Read32(r[5].UL[0]);
        f[2].UL = memory_.Read32(r[5].UL[0] + 4);
        f[1].f = Product(f[1].f, f[1].f);
        f[0].UL = memory_.Read32(r[5].UL[0] + 8);
        f[2].f = Product(f[2].f, f[2].f);
        r[1].SD[0] = 0x312bcc76;
        f[3].UL = 0x312bcc76;
        f[0].f = Product(f[0].f, f[0].f);
        f[1].f = Sum(f[1].f, f[2].f);
        f[0].f = Sum(f[1].f, f[0].f);
        const bool normalizable = f[3].f < f[0].f;
        auto& flags = context.fpuRegs.fprc[31];
        flags = (flags & ~0x800000u) | (normalizable ? 0x800000u : 0);
        if (normalizable) {
            r[1].SD[0] = 0x3f800000;
            f[1].UL = 0x3f800000;
            f[0].f = NearestRoot(f[0].f);
            f[0].f = NearestQuotient(f[1].f, f[0].f);
        } else f[0].UL = r[0].UL[0];
        f[2].f = Product(f[12].f, f[0].f);
    }
    // Preserve the complete NTSC operation's publication order: out/axis
    // aliases observe the scale and cosine writes before each axis reread.
    memory_.Write32(r[4].UL[0] + 8, f[2].UL);
    memory_.Write32(r[4].UL[0] + 4, f[2].UL);
    memory_.Write32(r[4].UL[0], f[2].UL);
    memory_.Write32(r[4].UL[0] + 12, f[13].UL);
    f[0].UL = memory_.Read32(r[5].UL[0]);
    f[0].f = Product(f[2].f, f[0].f);
    memory_.Write32(r[4].UL[0], f[0].UL);
    f[1].UL = memory_.Read32(r[5].UL[0] + 4);
    f[1].f = Product(f[2].f, f[1].f);
    memory_.Write32(r[4].UL[0] + 4, f[1].UL);
    f[0].UL = memory_.Read32(r[5].UL[0] + 8);
    f[0].f = Product(f[2].f, f[0].f);
    memory_.Write32(r[4].UL[0] + 8, f[0].UL);
}

void TrigMath::RotationAboutAxis(CpuContext& context) {
    calls_[22].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-48));
    const uint32_t stack = r[29].UL[0];
    r[1].SD[0] = 0x3f000000;
    f[0].UL = 0x3f000000;
    memory_.Write32(stack + 32, static_cast<uint32_t>(saved_return));
    memory_.Write32(stack + 36, static_cast<uint32_t>(saved_return >> 32));
    r[1].SD[0] = 0x38c90fdb;
    f[2].UL = 0x38c90fdb;
    f[1].f = static_cast<float>(static_cast<int32_t>(memory_.Read32(r[6].UL[0])));
    r[6].SD[0] = r[7].SD[0] + r[0].SD[0];
    f[1].f = Product(f[1].f, f[0].f);
    // A finite signed32 angle's half always fits signed32, including all
    // caller rounding modes. This is truncation, not integer right shift.
    r[2].SD[0] = static_cast<int32_t>(f[1].f);
    f[0].f = static_cast<float>(r[2].SL[0]);
    memory_.Write32(stack, r[2].UL[0]);
    f[0].f = Product(f[0].f, f[2].f);
    memory_.Write32(stack + 16, f[0].UL);
    EvaluateScratch(context, 16);
    f[12].UL = memory_.Read32(stack + 16);
    f[13].UL = memory_.Read32(stack + 20);
    r[31].UL[0] = 0x18d808;
    RotationFromAxisSine(context);
    r[31].UD[0] = saved_return;
    MoveStack(context, 48);
}

void TrigMath::InverseLength4(CpuContext& context) {
    calls_[25].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    f[0].UL = memory_.Read32(r[4].UL[0]);
    f[13].f = Product(f[13].f, f[13].f);
    f[3].UL = memory_.Read32(r[4].UL[0] + 4);
    f[0].f = Product(f[0].f, f[0].f);
    f[2].UL = memory_.Read32(r[4].UL[0] + 8);
    f[3].f = Product(f[3].f, f[3].f);
    f[1].UL = memory_.Read32(r[4].UL[0] + 12);
    f[2].f = Product(f[2].f, f[2].f);
    f[1].f = Product(f[1].f, f[1].f);
    f[0].f = Sum(f[0].f, f[3].f);
    f[0].f = Sum(f[0].f, f[2].f);
    f[1].f = Sum(f[0].f, f[1].f);
    const bool normalizable = f[13].f < f[1].f;
    auto& flags = context.fpuRegs.fprc[31];
    flags = (flags & ~0x800000u) | (normalizable ? 0x800000u : 0);
    f[0].UL = f[12].UL;
    if (normalizable) {
        r[1].SD[0] = 0x3f800000;
        f[0].f = NearestQuotient(1, NearestRoot(f[1].f));
    }
}

void TrigMath::SlerpRotations(CpuContext& context) {
    calls_[26].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved_output = r[16].UD[0], saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-32));
    const uint32_t stack = r[29].UL[0];
    f[7].UL = memory_.Read32(r[5].UL[0]);
    memory_.Write32(stack + 16, static_cast<uint32_t>(saved_output));
    memory_.Write32(stack + 20, static_cast<uint32_t>(saved_output >> 32));
    f[8].UL = f[12].UL;
    memory_.Write32(stack + 24, static_cast<uint32_t>(saved_return));
    memory_.Write32(stack + 28, static_cast<uint32_t>(saved_return >> 32));
    r[16].SD[0] = r[4].SD[0] + r[0].SD[0];
    f[0].UL = memory_.Read32(r[6].UL[0]);
    f[6].UL = memory_.Read32(r[5].UL[0] + 4);
    f[2].UL = memory_.Read32(r[6].UL[0] + 4);
    f[0].f = Product(f[7].f, f[0].f);
    f[5].UL = memory_.Read32(r[5].UL[0] + 8);
    f[2].f = Product(f[6].f, f[2].f);
    f[3].UL = memory_.Read32(r[6].UL[0] + 8);
    f[4].UL = memory_.Read32(r[5].UL[0] + 12);
    f[3].f = Product(f[5].f, f[3].f);
    f[1].UL = memory_.Read32(r[6].UL[0] + 12);
    f[0].f = Sum(f[0].f, f[2].f);
    f[1].f = Product(f[4].f, f[1].f);
    f[12].UL = r[0].UL[0];
    f[0].f = Sum(f[0].f, f[3].f);
    f[3].f = Sum(f[0].f, f[1].f);
    const bool opposite = f[3].f < f[12].f;
    auto& flags = context.fpuRegs.fprc[31];
    flags = (flags & ~0x800000u) | (opposite ? 0x800000u : 0);
    r[2].SD[0] = r[0].SD[0] + r[0].SD[0];
    if (opposite) {
        f[3].f = -f[3].f;
        r[2].SD[0] = static_cast<int32_t>(r[0].UL[0] + 1);
    }
    r[1].SD[0] = 0x3f800000;
    f[9].UL = 0x3f800000;
    r[1].SD[0] = 0x3d4ccccd;
    f[1].UL = 0x3d4ccccd;
    f[0].f = Difference(f[9].f, f[3].f);
    const bool curved = f[1].f < f[0].f;
    flags = (flags & ~0x800000u) | (curved ? 0x800000u : 0);
    if (curved) SlerpCurved(context);
    else {
        memory_.Write32(stack, f[7].UL);
        SlerpLinear(context);
    }
    r[16].UD[0] = saved_output;
    r[31].UD[0] = saved_return;
    MoveStack(context, 32);
}

void TrigMath::SlerpCurved(CpuContext& context) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t stack = r[29].UL[0], output = r[16].UL[0];
    const auto write = [&](uint32_t address, unsigned reg) { memory_.Write32(address, f[reg].UL); };
    auto& flags = context.fpuRegs.fprc[31];
    f[5].f = std::fabs(f[3].f);
    f[0].f = Difference(f[9].f, f[5].f);
    const bool below_zero = f[0].f < f[12].f;
    flags = (flags & ~0x800000u) | (below_zero ? 0x800000u : 0);
    if (below_zero) f[0].UL = f[12].UL;
    f[4].UL = 0xbc996e30;
    const bool negative = f[3].f < f[12].f;
    flags = (flags & ~0x800000u) | (negative ? 0x800000u : 0);
    f[1].UL = 0x3d981627;
    f[3].f = NearestRoot(f[0].f);
    f[4].f = Product(f[5].f, f[4].f);
    f[0].UL = 0x3e593484;
    r[1].SD[0] = 0x3fc90da4;
    f[2].UL = 0x3fc90da4;
    f[4].f = Sum(f[4].f, f[1].f);
    f[4].f = Product(f[4].f, f[5].f);
    f[4].f = Difference(f[4].f, f[0].f);
    f[4].f = Product(f[4].f, f[5].f);
    f[4].f = Sum(f[4].f, f[2].f);
    f[4].f = Product(f[4].f, f[3].f);
    if (negative) {
        r[1].SD[0] = 0x40490fdb;
        f[0].UL = 0x40490fdb;
        f[4].f = Difference(f[0].f, f[4].f);
    }
    f[1].UL = f[4].UL;
    write(stack, 1);
    EvaluateScratch(context, 0, false);
    f[0].f = Product(f[1].f, f[8].f);
    f[2].UL = f[0].UL;
    f[0].f = Difference(f[0].f, f[1].f);
    for (uint32_t i = 0; i < 4; ++i)
        memory_.Write32((stack & ~15u) + i * 4, context.vuRegs.VF[31].I[i]);
    f[3].UL = memory_.Read32(stack);
    write(stack, 2);
    write(stack + 8, 0);
    EvaluateScratch(context, 0, false);
    r[1].SD[0] = 0x3f800000;
    f[0].UL = 0x3f800000;
    f[3].f = NearestQuotient(f[0].f, f[3].f);
    for (uint32_t i = 0; i < 4; ++i)
        memory_.Write32((stack & ~15u) + i * 4, context.vuRegs.VF[31].I[i]);
    f[1].UL = memory_.Read32(stack);
    f[0].UL = memory_.Read32(stack + 8);
    f[8].f = Product(f[1].f, f[3].f);
    f[4].f = Product(f[0].f, f[3].f);
    if (r[2].SD[0] != r[0].SD[0]) {
        f[2].UL = memory_.Read32(r[5].UL[0] + 8);
        f[3].UL = memory_.Read32(r[5].UL[0] + 12);
        f[0].UL = memory_.Read32(r[5].UL[0] + 4);
        f[2].f = Product(f[2].f, f[4].f);
        f[1].UL = memory_.Read32(r[5].UL[0]);
        f[3].f = Product(f[3].f, f[4].f);
        f[0].f = Product(f[0].f, f[4].f);
        f[1].f = Product(f[1].f, f[4].f);
        write(output + 8, 2); write(output + 12, 3); write(output + 4, 0); write(output, 1);
        write(stack, 1); write(stack + 4, 0); write(stack + 8, 2); write(stack + 12, 3);
    } else {
        f[0].f = -f[4].f;
        f[3].UL = memory_.Read32(r[5].UL[0] + 8);
        f[4].UL = memory_.Read32(r[5].UL[0] + 12);
        f[2].UL = memory_.Read32(r[5].UL[0] + 4);
        f[1].UL = memory_.Read32(r[5].UL[0]);
        f[3].f = Product(f[3].f, f[0].f);
        f[4].f = Product(f[4].f, f[0].f);
        f[1].f = Product(f[1].f, f[0].f);
        f[2].f = Product(f[2].f, f[0].f);
        write(output + 8, 3); write(output + 12, 4); write(output, 1); write(output + 4, 2);
        write(stack, 1); write(stack + 4, 2); write(stack + 8, 3); write(stack + 12, 4);
    }
    FinishSlerpCurved(context);
}

void TrigMath::FinishSlerpCurved(CpuContext& context) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t stack = r[29].UL[0], output = r[16].UL[0];
    const auto write = [&](uint32_t address, unsigned reg) { memory_.Write32(address, f[reg].UL); };
    // The source reads to after publishing the first weighted vector.
    // Partial input/output aliases therefore participate in the operation.
    f[6].UL = memory_.Read32(r[6].UL[0] + 8);
    f[7].UL = memory_.Read32(r[6].UL[0] + 12);
    f[1].UL = memory_.Read32(r[6].UL[0] + 4);
    f[6].f = Product(f[6].f, f[8].f);
    f[0].UL = memory_.Read32(r[6].UL[0]);
    f[7].f = Product(f[7].f, f[8].f);
    f[1].f = Product(f[1].f, f[8].f);
    f[5].UL = memory_.Read32(output);
    f[0].f = Product(f[0].f, f[8].f);
    f[2].UL = memory_.Read32(output + 4);
    f[3].UL = memory_.Read32(output + 8);
    f[4].UL = memory_.Read32(output + 12);
    f[2].f = Sum(f[2].f, f[1].f);
    f[5].f = Sum(f[5].f, f[0].f);
    write(stack, 0);
    f[3].f = Sum(f[3].f, f[6].f);
    write(stack + 4, 1);
    f[4].f = Sum(f[4].f, f[7].f);
    write(output + 4, 2); write(output, 5); write(output + 8, 3); write(output + 12, 4);
    write(stack + 8, 6); write(stack + 12, 7);
}

void TrigMath::SlerpLinear(CpuContext& context) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    auto& flags = context.fpuRegs.fprc[31];
    const uint32_t stack = r[29].UL[0], output = r[16].UL[0];
    const auto write = [&](uint32_t address, unsigned reg) { memory_.Write32(address, f[reg].UL); };
    write(stack + 4, 6); write(stack + 8, 5); write(stack + 12, 4);
    if (r[2].SD[0] != r[0].SD[0]) {
        f[0].f = -f[7].f; f[1].f = -f[6].f; f[2].f = -f[5].f; f[3].f = -f[4].f;
        write(stack, 0); write(stack + 4, 1); write(stack + 8, 2); write(stack + 12, 3);
    }
    const bool positive = f[12].f < f[8].f;
    flags = (flags & ~0x800000u) | (positive ? 0x800000u : 0);
    f[1].UL = memory_.Read32(stack);
    if (!positive) {
        f[2].UL = memory_.Read32(stack + 4);
        f[3].UL = memory_.Read32(stack + 8);
        f[0].UL = memory_.Read32(stack + 12);
        write(output, 1); write(output + 12, 0); write(output + 4, 2); write(output + 8, 3);
        return;
    }
    f[3].UL = memory_.Read32(r[6].UL[0]);
    const bool partial = f[8].f < f[9].f;
    flags = (flags & ~0x800000u) | (partial ? 0x800000u : 0);
    write(output, 3);
    f[1].UL = memory_.Read32(r[6].UL[0] + 4);
    write(output + 4, 1);
    f[2].UL = memory_.Read32(r[6].UL[0] + 8);
    write(output + 8, 2);
    f[0].UL = memory_.Read32(r[6].UL[0] + 12);
    write(output + 12, 0);
    if (!partial) return;
    f[4].UL = memory_.Read32(stack + 12);
    r[4].SD[0] = r[16].SD[0] + r[0].SD[0];
    f[7].UL = memory_.Read32(stack);
    f[6].UL = memory_.Read32(stack + 4);
    f[0].f = Difference(f[0].f, f[4].f);
    f[5].UL = memory_.Read32(stack + 8);
    f[3].f = Difference(f[3].f, f[7].f);
    f[1].f = Difference(f[1].f, f[6].f);
    r[1].SD[0] = 0x2edbe6ff;
    f[13].UL = 0x2edbe6ff;
    f[2].f = Difference(f[2].f, f[5].f);
    f[3].f = Product(f[3].f, f[8].f);
    f[1].f = Product(f[1].f, f[8].f);
    f[0].f = Product(f[0].f, f[8].f);
    f[2].f = Product(f[2].f, f[8].f);
    f[3].f = Sum(f[3].f, f[7].f);
    f[1].f = Sum(f[1].f, f[6].f);
    f[2].f = Sum(f[2].f, f[5].f);
    f[0].f = Sum(f[0].f, f[4].f);
    write(output, 3); write(output + 4, 1); write(output + 8, 2); write(output + 12, 0);
    r[31].UL[0] = 0x18744c;
    InverseLength4(context);
    f[4].UL = memory_.Read32(output);
    f[2].UL = memory_.Read32(output + 4);
    f[3].UL = memory_.Read32(output + 8);
    f[4].f = Product(f[4].f, f[0].f);
    f[1].UL = memory_.Read32(output + 12);
    f[2].f = Product(f[2].f, f[0].f);
    f[3].f = Product(f[3].f, f[0].f);
    f[1].f = Product(f[1].f, f[0].f);
    write(output, 4); write(output + 4, 2); write(output + 8, 3); write(output + 12, 1);
}

void TrigMath::AngleFrom(CpuContext& context) {
    calls_[27].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const int64_t degrees = static_cast<int32_t>(r[0].UL[0] + 1);
    r[2].SD[0] = static_cast<int32_t>(r[0].UL[0] + 2);
    const uint32_t scale = r[5].SD[0] == degrees ? 0x43360b61 :
        r[5].SD[0] == r[2].SD[0] ? 0x47800000 : 0x4622f983;
    r[1].SD[0] = scale;
    f[0].f = Product(f[12].f, FromBits(scale));
    f[1].SL = TruncatedAngle(f[0].f);
    memory_.Write32(r[4].UL[0], f[1].UL);
}

void TrigMath::AngleOfPoint(CpuContext& context) {
    calls_[28].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    auto& flags = context.fpuRegs.fprc[31];
    f[0].f = Product(f[13].f, f[13].f);
    f[1].f = Product(f[12].f, f[12].f);
    f[2].UL = r[0].UL[0];
    f[0].f = Sum(f[0].f, f[1].f);
    f[0].f = NearestRoot(f[0].f);
    f[0].f = Sum(f[0].f, f[13].f);
    const bool positive = f[2].f < f[0].f;
    flags = (flags & ~0x800000u) | (positive ? 0x800000u : 0);
    r[2].UD[0] = r[0].UD[0] | 0x8000u;
    if (!positive) {
        memory_.Write32(r[4].UL[0], r[2].UL[0]);
        r[2].SD[0] = r[4].SD[0] + r[0].SD[0];
        return;
    }
    f[12].f = NearestQuotient(f[12].f, f[0].f);
    r[1].SD[0] = 0x3f800000;
    f[0].UL = 0x3f800000;
    f[6].f = Product(f[12].f, f[12].f);
    const bool small = f[6].f < f[0].f;
    flags = (flags & ~0x800000u) | (small ? 0x800000u : 0);
    PointAnglePolynomial(context, !small);
    r[1].SD[0] = 0x4622f983;
    f[3].UL = 0x4622f983;
    r[1].SD[0] = 0x38c90fdb;
    f[2].UL = 0x38c90fdb;
    f[0].f = Product(f[5].f, f[3].f);
    f[1].SL = TruncatedAngle(f[0].f);
    r[2].SD[0] = f[1].SL;
    f[1].f = static_cast<float>(f[1].SL);
    f[0].f = Product(f[1].f, f[2].f);
    f[0].f = Sum(f[0].f, f[0].f);
    f[0].f = Product(f[0].f, f[3].f);
    f[1].SL = TruncatedAngle(f[0].f);
    memory_.Write32(r[4].UL[0], f[1].UL);
    r[2].SD[0] = r[4].SD[0] + r[0].SD[0];
}

void TrigMath::PointAnglePolynomial(CpuContext& context, bool reciprocal) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    auto& flags = context.fpuRegs.fprc[31];
    if (reciprocal) f[0].f = NearestQuotient(f[0].f, f[12].f);
    f[5].UL = 0x3caaae5f;
    if (!reciprocal) {
        f[0].UL = 0x3dae5a36;
        f[5].f = Product(f[6].f, f[5].f);
        f[1].UL = 0x3e3876e2;
        f[2].UL = 0x3ea91d04;
        f[3].UL = 0x3f7ff738;
        r[1].SD[0] = 0x3f7ff738;
        f[5].f = Difference(f[5].f, f[0].f);
        f[5].f = Sum(Product(f[5].f, f[6].f), f[1].f);
        f[5].f = Difference(Product(f[5].f, f[6].f), f[2].f);
        f[5].f = Sum(Product(f[5].f, f[6].f), f[3].f);
        f[5].f = Product(f[5].f, f[12].f);
        return;
    }
    f[1].UL = 0x3dae5a36;
    const bool positive = f[2].f < f[12].f;
    flags = (flags & ~0x800000u) | (positive ? 0x800000u : 0);
    f[2].UL = 0x3e3876e2;
    f[3].UL = 0x3ea91d04;
    f[4].UL = 0x3f7ff738;
    f[6].f = Product(f[0].f, f[0].f);
    f[5].f = Difference(Product(f[6].f, f[5].f), f[1].f);
    f[5].f = Sum(Product(f[5].f, f[6].f), f[2].f);
    f[5].f = Difference(Product(f[5].f, f[6].f), f[3].f);
    f[5].f = Sum(Product(f[5].f, f[6].f), f[4].f);
    f[5].f = Product(f[5].f, f[0].f);
    r[1].SD[0] = static_cast<int32_t>(positive ? 0x3fc90fdb : 0xbfc90fdb);
    f[0].UL = r[1].UL[0];
    f[5].f = Difference(f[0].f, f[5].f);
}

std::array<int32_t, 3> TrigMath::EvaluateEulerAngles(const Matrix& matrix) {
    const float near = FromBits(0x3a03126f);
    if (std::fabs(matrix[0][1]) <= near && std::fabs(matrix[1][0]) <= near &&
        std::fabs(matrix[1][2]) <= near && std::fabs(matrix[2][1]) <= near &&
        std::fabs(Difference(1, matrix[1][1])) <= near &&
        std::fabs(Difference(matrix[0][0], matrix[2][2])) <= near &&
        std::fabs(Sum(matrix[0][2], matrix[2][0])) <= near)
        return {0, EvaluateAngleOfPoint(matrix[2][0], matrix[0][0]), 0};
    const float sine = -matrix[0][2];
    float radians = Difference(FromBits(0x3fc90fdb), ArcCosine(std::fabs(sine)));
    if (sine < 0) radians = -radians;
    const int32_t yaw = EvaluateAngleFrom(radians, 0);
    if (yaw < 16384) {
        if (yaw > -16384)
            return {EvaluateAngleOfPoint(matrix[1][2], matrix[2][2]), yaw,
                    EvaluateAngleOfPoint(matrix[0][1], matrix[0][0])};
        return {0, yaw, EvaluateAngleOfPoint(-matrix[1][0], -matrix[2][0])};
    }
    const auto roll = static_cast<uint32_t>(EvaluateAngleOfPoint(matrix[1][0], matrix[2][0]));
    return {0, yaw, static_cast<int32_t>(0u - roll)};
}

void TrigMath::EulerAnglesOfMatrix(CpuContext& context) {
    calls_[29].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved16 = r[16].UD[0], saved17 = r[17].UD[0];
    const uint64_t saved18 = r[18].UD[0], saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-64));
    const uint32_t stack = r[29].UL[0];
    const auto save = [&](uint32_t offset, uint64_t value) {
        memory_.Write32(stack + offset, static_cast<uint32_t>(value));
        memory_.Write32(stack + offset + 4, static_cast<uint32_t>(value >> 32));
    };
    r[1].SD[0] = 0x3a03126f;
    f[2].UL = 0x3a03126f;
    save(16, saved16);
    r[2].SD[0] = static_cast<int32_t>(r[0].UL[0] + 1);
    save(24, saved17);
    r[16].SD[0] = r[4].SD[0] + r[0].SD[0];
    save(32, saved18);
    r[4].SD[0] = r[6].SD[0] + r[0].SD[0];
    save(40, saved_return);
    r[18].SD[0] = r[5].SD[0] + r[0].SD[0];
    memory_.Write32(stack + 48, f[20].UL);
    if (EulerYawOnly(context)) {
        r[31].UL[0] = 0x1852f0;
        AngleOfPoint(context);
        f[12].UL = r[0].UL[0];
        r[4].SD[0] = r[17].SD[0] + r[0].SD[0];
        r[5].SD[0] = r[0].SD[0] + r[0].SD[0];
        r[31].UL[0] = 0x185300;
        AngleFrom(context);
        r[2].SD[0] = static_cast<int32_t>(memory_.Read32(r[17].UL[0]));
        memory_.Write32(r[18].UL[0], r[2].UL[0]);
        memory_.Write32(stack, r[2].UL[0]);
    } else {
        f[1].f = -f[3].f;
        EulerGeneralYaw(context);
        EulerOuterAngles(context);
    }
    r[16].UD[0] = saved16; r[17].UD[0] = saved17;
    r[18].UD[0] = saved18; r[31].UD[0] = saved_return;
    // Unlike the integer registers, this source save is read back from the
    // stack. An output alias can change the restored FPR20 value.
    f[20].UL = memory_.Read32(stack + 48);
    MoveStack(context, 64);
}

bool TrigMath::EulerYawOnly(CpuContext& context) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    auto& flags = context.fpuRegs.fprc[31];
    const uint32_t matrix = r[16].UL[0];
    f[0].f = std::fabs(FromBits(memory_.Read32(matrix + 4)));
    const bool first_near = f[0].f <= f[2].f;
    flags = (flags & ~0x800000u) | (first_near ? 0x800000u : 0);
    r[17].SD[0] = r[7].SD[0] + r[0].SD[0];
    if (!first_near) r[2].SD[0] = r[0].SD[0] + r[0].SD[0];
    r[2].UD[0] &= 0xffu;
    f[3].UL = memory_.Read32(matrix + 8);
    if (r[2].SD[0] == r[0].SD[0]) return false;
    const auto close = [&] {
        const bool near = f[0].f <= f[2].f;
        flags = (flags & ~0x800000u) | (near ? 0x800000u : 0);
        r[2].SD[0] = near ? static_cast<int32_t>(r[0].UL[0] + 1) : r[0].SD[0] + r[0].SD[0];
        r[2].UD[0] &= 0xffu;
        return r[2].SD[0] != r[0].SD[0];
    };
    for (const uint32_t offset : {16u, 24u, 36u}) {
        f[0].f = std::fabs(FromBits(memory_.Read32(matrix + offset)));
        if (!close()) return false;
    }
    f[1].UL = memory_.Read32(matrix + 20);
    r[1].SD[0] = 0x3f800000;
    f[0].f = std::fabs(Difference(1, f[1].f));
    if (!close()) return false;
    f[13].UL = memory_.Read32(matrix);
    f[0].UL = memory_.Read32(matrix + 40);
    f[0].f = std::fabs(Difference(f[13].f, f[0].f));
    if (!close()) return false;
    f[12].UL = memory_.Read32(matrix + 32);
    f[0].f = std::fabs(Sum(f[3].f, f[12].f));
    return close();
}

void TrigMath::EulerGeneralYaw(CpuContext& context) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    auto& flags = context.fpuRegs.fprc[31];
    r[1].SD[0] = 0x3f800000;
    f[0].UL = 0x3f800000;
    f[20].UL = r[0].UL[0];
    f[6].f = std::fabs(f[1].f);
    f[0].f = Difference(f[0].f, f[6].f);
    const bool below_zero = f[0].f < f[20].f;
    flags = (flags & ~0x800000u) | (below_zero ? 0x800000u : 0);
    if (below_zero) f[0].UL = f[20].UL;
    f[5].UL = 0xbc996e30;
    const bool negative = f[1].f < f[20].f;
    flags = (flags & ~0x800000u) | (negative ? 0x800000u : 0);
    f[2].UL = 0x3d981627;
    f[0].f = NearestRoot(f[0].f);
    f[5].f = Product(f[6].f, f[5].f);
    f[1].UL = 0x3e593484;
    f[3].UL = 0x3fc90da4;
    r[1].SD[0] = 0x3fc90fdb;
    f[4].UL = 0x3fc90fdb;
    f[5].f = Sum(f[5].f, f[2].f);
    f[5].f = Difference(Product(f[5].f, f[6].f), f[1].f);
    f[5].f = Sum(Product(f[5].f, f[6].f), f[3].f);
    f[0].f = Product(f[0].f, f[5].f);
    f[5].f = Difference(f[4].f, f[0].f);
    if (negative) f[5].f = -f[5].f;
    r[1].SD[0] = 0x4622f983;
    f[0].f = Product(f[5].f, FromBits(0x4622f983));
    f[1].SL = TruncatedAngle(f[0].f);
    r[3].SD[0] = f[1].SL;
    r[2].UD[0] = r[3].SD[0] < 16384 ? 1 : 0;
    memory_.Write32(r[4].UL[0], r[3].UL[0]);
}

void TrigMath::EulerOuterAngles(CpuContext& context) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    if (r[2].SD[0] == r[0].SD[0]) {
        f[12].UL = memory_.Read32(r[16].UL[0] + 16);
        r[4].SD[0] = r[17].SD[0] + r[0].SD[0];
        f[13].UL = memory_.Read32(r[16].UL[0] + 32);
        r[31].UL[0] = 0x185444;
        AngleOfPoint(context);
        r[3].SD[0] = static_cast<int32_t>(memory_.Read32(r[17].UL[0]));
        r[4].SD[0] = r[18].SD[0] + r[0].SD[0];
        f[12].UL = f[20].UL;
        r[5].SD[0] = r[0].SD[0] + r[0].SD[0];
        r[3].SD[0] = static_cast<int32_t>(0u - r[3].UL[0]);
        memory_.Write32(r[17].UL[0], r[3].UL[0]);
        r[31].UL[0] = 0x185460;
        AngleFrom(context);
        return;
    }
    r[2].SD[0] = static_cast<int32_t>(r[0].UL[0] - 16384u);
    r[2].UD[0] = r[2].SD[0] < r[3].SD[0] ? 1 : 0;
    const bool lower = r[2].SD[0] == r[0].SD[0];
    r[4].SD[0] = r[17].SD[0] + r[0].SD[0];
    if (lower) {
        f[13].f = -FromBits(memory_.Read32(r[16].UL[0] + 32));
        f[12].f = -FromBits(memory_.Read32(r[16].UL[0] + 16));
        r[31].UL[0] = 0x18541c;
        AngleOfPoint(context);
        r[4].SD[0] = r[18].SD[0] + r[0].SD[0];
        f[12].UL = f[20].UL;
        r[5].SD[0] = r[0].SD[0] + r[0].SD[0];
        r[31].UL[0] = 0x18542c;
        AngleFrom(context);
        return;
    }
    f[12].UL = memory_.Read32(r[16].UL[0] + 4);
    f[13].UL = memory_.Read32(r[16].UL[0]);
    r[31].UL[0] = 0x1853f0;
    AngleOfPoint(context);
    f[13].UL = memory_.Read32(r[16].UL[0] + 40);
    r[4].SD[0] = r[18].SD[0] + r[0].SD[0];
    f[12].UL = memory_.Read32(r[16].UL[0] + 24);
    r[31].UL[0] = 0x185400;
    AngleOfPoint(context);
}

TrigMath::Vector TrigMath::EvaluateSnapToYaw(const Vector& rotation, float step) {
    const auto rows = MathLibrary::EvaluateRotationMatrix(rotation).rows;
    const Matrix matrix{rows[0], rows[1], rows[2], {}};
    const int32_t yaw = EvaluateEulerAngles(matrix)[1];
    const float biased = Sum(AngleRadians(yaw), Product(step, 0.5f));
    const int32_t steps = TruncatedAngle(NearestQuotient(biased, step));
    const int32_t snapped = EvaluateAngleFrom(Product(step, static_cast<float>(steps)), 0);
    return EvaluateRotation(snapped, Axis::Yaw);
}

void TrigMath::SnapToYaw(CpuContext& context) {
    calls_[30].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved16 = r[16].UD[0], saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-160));
    const uint32_t stack = r[29].UL[0];
    memory_.Write32(stack + 112, static_cast<uint32_t>(saved16));
    memory_.Write32(stack + 116, static_cast<uint32_t>(saved16 >> 32));
    memory_.Write32(stack + 128, f[20].UL);
    r[16].SD[0] = r[4].SD[0] + r[0].SD[0];
    f[20].UL = f[12].UL;
    memory_.Write32(stack + 144, f[22].UL);
    memory_.Write32(stack + 136, f[21].UL);
    r[4].SD[0] = static_cast<int32_t>(r[29].UL[0] + 48);
    memory_.Write32(stack + 120, static_cast<uint32_t>(saved_return));
    memory_.Write32(stack + 124, static_cast<uint32_t>(saved_return >> 32));
    r[5].SD[0] = r[16].SD[0] + r[0].SD[0];
    r[31].UL[0] = 0x1874e4;
    matrices_.RotationMatrix(context);
    r[1].SD[0] = 0x38c90fdb;
    f[22].UL = 0x38c90fdb;
    r[4].SD[0] = static_cast<int32_t>(r[29].UL[0] + 48);
    r[5].SD[0] = r[29].SD[0] + r[0].SD[0];
    r[6].SD[0] = static_cast<int32_t>(r[29].UL[0] + 16);
    r[7].SD[0] = static_cast<int32_t>(r[29].UL[0] + 32);
    r[31].UL[0] = 0x187504;
    EulerAnglesOfMatrix(context);
    r[1].SD[0] = 0x3f000000;
    f[21].UL = 0x3f000000;
    f[0].UL = memory_.Read32(stack + 16);
    f[0].f = static_cast<float>(f[0].SL);
    r[4].SD[0] = static_cast<int32_t>(r[29].UL[0] + 16);
    f[1].f = Product(f[20].f, f[21].f);
    r[5].SD[0] = r[0].SD[0] + r[0].SD[0];
    f[0].f = Product(f[0].f, f[22].f);
    f[0].f = Sum(f[0].f, f[1].f);
    f[0].f = NearestQuotient(f[0].f, f[20].f);
    f[1].SL = TruncatedAngle(f[0].f);
    r[2].SD[0] = f[1].SL;
    f[12].f = static_cast<float>(r[2].SL[0]);
    f[12].f = Product(f[20].f, f[12].f);
    r[31].UL[0] = 0x187550;
    AngleFrom(context);
    r[2].SD[0] = static_cast<int32_t>(memory_.Read32(stack + 16));
    memory_.Write32(stack + 48, r[2].UL[0]);
    if (r[2].SD[0] == 0) {
        r[1].SD[0] = 0x3f800000;
        f[0].UL = 0x3f800000;
        memory_.Write32(r[16].UL[0] + 8, r[0].UL[0]);
        memory_.Write32(r[16].UL[0] + 12, f[0].UL);
        memory_.Write32(r[16].UL[0] + 4, r[0].UL[0]);
        memory_.Write32(r[16].UL[0], r[0].UL[0]);
    } else {
        f[0].f = static_cast<float>(r[2].SL[0]);
        f[0].f = Product(f[0].f, f[22].f);
        f[0].f = Product(f[0].f, f[21].f);
        memory_.Write32(stack + 64, f[0].UL);
        EvaluateScratch(context, 64);
        f[0].UL = memory_.Read32(stack + 68);
        f[1].UL = memory_.Read32(stack + 64);
        memory_.Write32(r[16].UL[0] + 12, f[0].UL);
        memory_.Write32(r[16].UL[0] + 4, f[1].UL);
        memory_.Write32(r[16].UL[0], r[0].UL[0]);
        memory_.Write32(r[16].UL[0] + 8, r[0].UL[0]);
    }
    r[16].UD[0] = saved16;
    r[31].UD[0] = saved_return;
    f[22].UL = memory_.Read32(stack + 144);
    f[21].UL = memory_.Read32(stack + 136);
    f[20].UL = memory_.Read32(stack + 128);
    MoveStack(context, 160);
}

}  // namespace title
