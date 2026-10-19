#include "games/slus_20909_crash_twinsanity/hle/math/trig_math.h"

#include <cfenv>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

#include "engine/ee/cpu/ps2_float.h"
#include "engine/ipu/hle/guest_memory.h"
#include "engine/platform/host/platform.h"
#include "engine/vu/vu0/lib_vu0_sin_cos.h"

// EvaluateSinCos sets FE_TOWARDZERO around SineCosine: its FP operations depend on
// the dynamic rounding mode (no folding, no hoisting across fesetround).
PS2R_FENV_ACCESS_ON

namespace title {

float TrigMath::FromBits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint32_t TrigMath::Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

// This is the input/output policy of the already accepted host trig
// operation. It does not depend on a VU class, program image or interpreter.
float TrigMath::TrigArgument(float value) {
    uint32_t bits = Bits(value);
    const uint32_t exponent = bits & 0x7f800000u;
    if (exponent == 0) bits &= 0x80000000u;
    else if (exponent == 0x7f800000u) bits = (bits & 0x80000000u) | 0x7f7fffffu;
    return FromBits(bits);
}

void TrigMath::MoveStack(CpuContext& context, uint32_t amount) {
    auto& stack = context.cpuRegs.GPR.r[29];
    stack.SD[0] = static_cast<int32_t>(stack.UL[0] + amount);
}

float TrigMath::Product(float a, float b) {
    volatile float result = ps2_float_clamp(a) * ps2_float_clamp(b);
    return ps2_float_clamp(result);
}

float TrigMath::Sum(float a, float b) {
    volatile float result = ps2_float_clamp(a) + ps2_float_clamp(b);
    return ps2_float_clamp(result);
}

float TrigMath::Difference(float a, float b) {
    volatile float result = ps2_float_clamp(a) - ps2_float_clamp(b);
    return ps2_float_clamp(result);
}

float TrigMath::NearestQuotient(float numerator, float denominator) {
    const int saved = std::fegetround();
    if (saved != FE_TONEAREST) (void)std::fesetround(FE_TONEAREST);
    volatile float result = ps2_float_clamp(numerator) / ps2_float_clamp(denominator);
    if (saved != FE_TONEAREST) (void)std::fesetround(saved);
    return ps2_float_clamp(result);
}

float TrigMath::NearestRoot(float value) {
    const int saved = std::fegetround();
    if (saved != FE_TONEAREST) (void)std::fesetround(FE_TONEAREST);
    volatile float operand = std::fabs(ps2_float_clamp(value));
    volatile float root = std::sqrt(operand);
    if (saved != FE_TONEAREST) (void)std::fesetround(saved);
    return ps2_float_clamp(root);
}

float TrigMath::ArcCosine(float cosine) {
    const float magnitude = std::fabs(cosine);
    float rest = Difference(1, magnitude);
    if (rest < 0) rest = 0;
    float factor = Sum(Product(magnitude, FromBits(0xbc996e30)), FromBits(0x3d981627));
    factor = Difference(Product(factor, magnitude), FromBits(0x3e593484));
    factor = Sum(Product(factor, magnitude), FromBits(0x3fc90da4));
    const float angle = Product(factor, NearestRoot(rest));
    return cosine < 0 ? Difference(FromBits(0x40490fdb), angle) : angle;
}

int32_t TrigMath::TruncatedAngle(float value) {
    if (value >= 2147483648.0f) return 2147483647;
    if (value < -2147483648.0f) return -2147483647 - 1;
    return static_cast<int32_t>(value);
}

float TrigMath::PointAngleEstimate(float tangent) {
    const float squared = Product(tangent, tangent);
    float value = Difference(Product(squared, FromBits(0x3caaae5f)), FromBits(0x3dae5a36));
    value = Sum(Product(value, squared), FromBits(0x3e3876e2));
    value = Difference(Product(value, squared), FromBits(0x3ea91d04));
    value = Sum(Product(value, squared), FromBits(0x3f7ff738));
    return Product(value, tangent);
}

TrigMath::Vector TrigMath::ComposeAngles(const TrigMath::Vector& values, TrigMath::AxisPair axes) {
    const float first = Product(values[0], values[3]);
    const float second = Product(values[1], values[2]);
    const float cross = Product(axes == TrigMath::AxisPair::XZ ? values[0] : -values[0], values[2]);
    const float scalar = Product(values[1], values[3]);
    if (axes == TrigMath::AxisPair::XY) return {first, second, cross, scalar};
    if (axes == TrigMath::AxisPair::XZ) return {first, cross, second, scalar};
    return {cross, first, second, scalar};
}

TrigMath::TrigMath(GuestMemory& memory)
    : memory_(memory), matrices_(memory),
      generated_body_calls_(std::make_shared<std::array<std::atomic<uint64_t>, kEntryCount>>()) {}


TrigMath::Vector TrigMath::EvaluateSinCos(float first, float second) {
    const int saved = std::fegetround();
    if (saved != FE_TOWARDZERO) (void)std::fesetround(FE_TOWARDZERO);
    first = TrigArgument(first);
    second = TrigArgument(second);
    // The ONE host sin/cos (LibVu0SinCos::SineCosine): the same bits as the VU0
    // substitution the production oracle models, on every host (not the libm,
    // whose sinf/cosf differ by one code between Apple and glibc).
    float sine_first, cosine_first, sine_second, cosine_second;
    LibVu0SinCos::SineCosine(first, sine_first, cosine_first);
    LibVu0SinCos::SineCosine(second, sine_second, cosine_second);
    const Vector result{
        TrigArgument(sine_first), TrigArgument(cosine_first),
        TrigArgument(sine_second), TrigArgument(cosine_second)};
    if (saved != FE_TOWARDZERO) (void)std::fesetround(saved);
    return result;
}

float TrigMath::AngleRadians(int32_t angle) {
    volatile float converted = static_cast<float>(angle);
    volatile float radians = converted * FromBits(0x38c90fdbu);
    return radians;
}

TrigMath::Matrix TrigMath::EvaluateAxisMatrix(int32_t angle, Axis axis) {
    auto result = MathLibrary::EvaluateIdentity();
    if (angle == 0) {
        // The source identity operation clears its first two rows by
        // subtraction. Preserve their negative zeros under downward rounding;
        // the last two rows are raw moves of canonical vector constants.
        volatile float clear_input = 0;
        const float cleared = Difference(clear_input, clear_input);
        for (size_t row = 0; row < 2; ++row)
            for (size_t lane = 0; lane < 4; ++lane)
                if (lane != row) result[row][lane] = cleared;
        return result;
    }
    const auto values = EvaluateSinCos(AngleRadians(angle), 0);
    const size_t first = axis == Axis::Pitch ? 1 : 0;
    const size_t second = axis == Axis::Roll ? 1 : 2;
    const float sine = axis == Axis::Yaw ? -values[0] : values[0];
    result[first][first] = result[second][second] = values[1];
    result[first][second] = sine;
    result[second][first] = -sine;
    return result;
}

float TrigMath::EvaluateTangent(int32_t angle) {
    const auto values = EvaluateSinCos(AngleRadians(angle), 0);
    if (values[1] == 0) return FromBits(values[0] > 0 ? 0x7149f2cau : 0xf149f2cau);
    return NearestQuotient(values[0], values[1]);
}

TrigMath::Matrix TrigMath::EvaluateMatrixPair(int32_t first, int32_t second, AxisPair axes) {
    const Axis first_axis = axes == AxisPair::YZ ? Axis::Yaw : Axis::Pitch;
    const Axis second_axis = axes == AxisPair::XY ? Axis::Yaw : Axis::Roll;
    if (first == 0) return EvaluateAxisMatrix(second, second_axis);
    if (second == 0) return EvaluateAxisMatrix(first, first_axis);
    const auto values = EvaluateSinCos(AngleRadians(first), AngleRadians(second));
    const float sa = values[0], ca = values[1], sb = values[2], cb = values[3];
    if (axes == AxisPair::XY)
        return {{{cb, 0, -sb, 0}, {Product(sb, sa), ca, Product(sa, cb), 0},
            {Product(sb, ca), -sa, Product(ca, cb), 0}, {0, 0, 0, 1}}};
    if (axes == AxisPair::XZ)
        return {{{cb, sb, 0, 0}, {Product(-ca, sb), Product(ca, cb), sa, 0},
            {Product(sa, sb), Product(-sa, cb), ca, 0}, {0, 0, 0, 1}}};
    return {{{Product(ca, cb), Product(ca, sb), -sa, 0}, {-sb, cb, 0, 0},
        {Product(sa, cb), Product(sa, sb), ca, 0}, {0, 0, 0, 1}}};
}

TrigMath::Matrix TrigMath::EvaluateMatrixXYZ(int32_t x, int32_t y, int32_t z) {
    if (x == 0) return EvaluateMatrixPair(y, z, AxisPair::YZ);
    if (y == 0) return EvaluateMatrixPair(x, z, AxisPair::XZ);
    if (z == 0) return EvaluateMatrixPair(x, y, AxisPair::XY);
    const auto xz = EvaluateSinCos(AngleRadians(x), AngleRadians(z));
    const auto yy = EvaluateSinCos(AngleRadians(y), 0);
    const float cc = Product(xz[1], xz[3]), cs = Product(xz[1], xz[2]);
    const float ss = Product(xz[0], xz[2]), sc = Product(xz[0], xz[3]);
    return {{{Product(yy[1], xz[3]), Product(yy[1], xz[2]), -yy[0], 0},
        {Difference(Product(yy[0], sc), cs), Sum(Product(yy[0], ss), cc), Product(xz[0], yy[1]), 0},
        {Sum(Product(yy[0], cc), ss), Difference(Product(yy[0], cs), sc), Product(xz[1], yy[1]), 0},
        {0, 0, 0, 1}}};
}

TrigMath::Matrix TrigMath::EvaluateMatrixAboutAxis(
    float sine, float cosine, const Vector& axis, bool unit_axis) {
    float x = axis[0], y = axis[1], z = axis[2];
    if (!unit_axis) {
        const float squared = Sum(Sum(Product(x, x), Product(y, y)), Product(z, z));
        const float scale = FromBits(0x312bcc76) < squared ? NearestQuotient(1, NearestRoot(squared)) : 0;
        z = Product(z, scale);
        x = Product(x, scale);
        y = Product(y, scale);
    }
    const float rest = Difference(1, cosine);
    const float rest_x = Product(rest, x), rest_z = Product(rest, z), rest_y = Product(rest, y);
    const float xz = Product(rest_x, z), xy = Product(rest_x, y), yz = Product(rest_y, z);
    const float zz = Product(rest_z, z), xx = Product(rest_x, x), yy = Product(rest_y, y);
    const float sine_z = Product(sine, z), sine_x = Product(sine, x), sine_y = Product(sine, y);
    return {{{Sum(xx, cosine), Sum(xy, sine_z), Difference(xz, sine_y), 0},
             {Difference(xy, sine_z), Sum(yy, cosine), Sum(yz, sine_x), 0},
             {Sum(xz, sine_y), Difference(yz, sine_x), Sum(zz, cosine), 0},
             {0, 0, 0, 1}}};
}

TrigMath::Matrix TrigMath::EvaluateAxisAngleMatrix(int32_t angle, const Vector& axis, bool unit_axis) {
    if (angle == 0) return EvaluateAxisMatrix(0, Axis::Pitch);
    const auto values = EvaluateSinCos(AngleRadians(angle), 0);
    return EvaluateMatrixAboutAxis(values[0], values[1], axis, unit_axis);
}

void TrigMath::EvaluateScratch(CpuContext& context, uint32_t offset, bool store) {
    auto& v = context.vuRegs;
    // Scalar stores use the exact stack address; quad loads/stores align down.
    // Preserve stale y/w words and the scratch second angle in the legacy ABI.
    const uint32_t base = (context.cpuRegs.GPR.r[29].UL[0] + offset) & ~15u;
    for (uint32_t i = 0; i < 4; ++i) v.VF[20].I[i] = memory_.Read32(base + i * 4);
    const auto values = EvaluateSinCos(v.VF[20].x, v.VF[20].z);
    std::memcpy(v.VF[31].F, values.data(), 16);
    v.VF[0] = {};
    v.VF[0].w = 1;
    v.VI[0] = 0;
    v.TPC = 0x1c0;
    if (store) for (uint32_t i = 0; i < 4; ++i) memory_.Write32(base + i * 4, v.VF[31].I[i]);
}

void TrigMath::CosSin16(CpuContext& context) {
    calls_[0].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const int32_t angle = static_cast<int32_t>(memory_.Read32(r[4].UL[0]));
    r[1].SD[0] = 0x38c90fdb;
    f[1].UL = 0x38c90fdb;
    MoveStack(context, uint32_t(-16));
    f[0].f = AngleRadians(angle);
    memory_.Write32(r[29].UL[0], f[0].UL);
    EvaluateScratch(context);
    f[0].UL = memory_.Read32(r[29].UL[0] + 4);
    f[1].UL = memory_.Read32(r[29].UL[0]);
    memory_.Write32(r[5].UL[0], f[0].UL);
    memory_.Write32(r[6].UL[0], f[1].UL);
    MoveStack(context, 16);
}

void TrigMath::SinCos16(CpuContext& context) {
    calls_[1].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    r[1].SD[0] = 0x38c90fdb;
    f[1].UL = 0x38c90fdb;
    MoveStack(context, uint32_t(-16));
    f[0].f = AngleRadians(r[4].SL[0]);
    memory_.Write32(r[29].UL[0] + 8, f[0].UL);
    memory_.Write32(r[29].UL[0], f[0].UL);
    EvaluateScratch(context);
    f[0].UL = memory_.Read32(r[29].UL[0]);
    f[1].UL = memory_.Read32(r[29].UL[0] + 4);
    memory_.Write32(r[5].UL[0], f[0].UL);
    memory_.Write32(r[5].UL[0] + 4, f[1].UL);
    MoveStack(context, 16);
}

void TrigMath::SinCos16Pair(CpuContext& context) {
    calls_[2].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    r[1].SD[0] = 0x38c90fdb;
    f[2].UL = 0x38c90fdb;
    MoveStack(context, uint32_t(-16));
    f[1].f = AngleRadians(r[4].SL[0]);
    f[0].f = AngleRadians(r[5].SL[0]);
    memory_.Write32(r[29].UL[0], f[1].UL);
    memory_.Write32(r[29].UL[0] + 8, f[0].UL);
    EvaluateScratch(context);
    f[1].UL = memory_.Read32(r[29].UL[0]);
    f[2].UL = memory_.Read32(r[29].UL[0] + 4);
    f[3].UL = memory_.Read32(r[29].UL[0] + 8);
    f[0].UL = memory_.Read32(r[29].UL[0] + 12);
    memory_.Write32(r[6].UL[0], f[1].UL);
    memory_.Write32(r[6].UL[0] + 12, f[0].UL);
    memory_.Write32(r[6].UL[0] + 4, f[2].UL);
    memory_.Write32(r[6].UL[0] + 8, f[3].UL);
    MoveStack(context, 16);
}

void TrigMath::AxisMatrix(CpuContext& context, Axis axis) {
    const size_t index = static_cast<size_t>(axis);
    calls_[9 + index].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-32));
    memory_.Write32(r[29].UL[0] + 16, static_cast<uint32_t>(saved_return));
    memory_.Write32(r[29].UL[0] + 20, static_cast<uint32_t>(saved_return >> 32));
    r[5].SD[0] = static_cast<int32_t>(memory_.Read32(r[5].UL[0]));
    if (r[5].SD[0] == 0) {
        constexpr uint32_t returns[] = {0x18bb04, 0x18bbdc, 0x18bcb4};
        r[31].UL[0] = returns[index];
        // Reuse the proved host identity operation. No generated call or VU
        // model is involved; the enclosing trig entry owns its call count.
        matrices_.IdentityMatrix(context);
    } else {
        r[1].SD[0] = 0x38c90fdb;
        f[1].UL = 0x38c90fdb;
        f[0].f = AngleRadians(r[5].SL[0]);
        memory_.Write32(r[29].UL[0], f[0].UL);
        EvaluateScratch(context, 0, false);
        FinishAxisMatrix(context, axis, 0);
    }
    // Full NTSC entries restore the saved host return even if a matrix store
    // overwrote the guest stack backup. Interior entries remain delegated.
    r[31].UD[0] = saved_return;
    MoveStack(context, 32);
}

void TrigMath::MatrixAboutX(CpuContext& context) { AxisMatrix(context, Axis::Pitch); }
void TrigMath::MatrixAboutY(CpuContext& context) { AxisMatrix(context, Axis::Yaw); }
void TrigMath::MatrixRotationZ(CpuContext& context) { AxisMatrix(context, Axis::Roll); }

void TrigMath::FinishAxisMatrix(CpuContext& context, Axis axis, uint32_t scratch_offset) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    r[1].SD[0] = 0x3f800000;
    f[3].UL = 0x3f800000;
    const uint32_t output = r[4].UL[0];
    // Source clears the matrix before publishing the trig scratch.
    // Descending stores within rows are observable when output aliases SP.
    constexpr uint32_t clear[] = {12, 8, 4, 0, 28, 24, 20, 16, 44, 40, 36, 32, 56, 52, 48};
    for (const auto offset : clear) memory_.Write32(output + offset, r[0].UL[0]);
    memory_.Write32(output + 60, f[3].UL);
    const uint32_t scratch = r[29].UL[0] + scratch_offset;
    for (uint32_t i = 0; i < 4; ++i)
        memory_.Write32((scratch & ~15u) + i * 4, context.vuRegs.VF[31].I[i]);
    f[2].UL = memory_.Read32(scratch);
    f[0].UL = memory_.Read32(scratch + 4);
    f[1].f = -f[2].f;
    constexpr uint32_t destinations[][5] = {
        {0, 20, 40, 36, 24}, {20, 0, 40, 8, 32}, {40, 0, 20, 16, 4}};
    constexpr unsigned registers[] = {3, 0, 0, 1, 2};
    for (unsigned i = 0; i < 5; ++i)
        memory_.Write32(output + destinations[static_cast<size_t>(axis)][i], f[registers[i]].UL);
}

void TrigMath::MatrixPair(CpuContext& context, AxisPair axes) {
    const size_t index = static_cast<size_t>(axes);
    calls_[14 + index].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-64));
    const uint32_t stack = r[29].UL[0];
    memory_.Write32(stack + 48, static_cast<uint32_t>(saved_return));
    memory_.Write32(stack + 52, static_cast<uint32_t>(saved_return >> 32));
    r[5].SD[0] = static_cast<int32_t>(memory_.Read32(r[5].UL[0]));
    r[6].SD[0] = static_cast<int32_t>(memory_.Read32(r[6].UL[0]));
    if (r[5].SD[0] == 0) memory_.Write32(stack, r[6].UL[0]);
    if (r[5].SD[0] == 0 && r[6].SD[0] == 0) {
        constexpr uint32_t returns[] = {0x182af4, 0x182d64, 0x182fd4};
        r[31].UL[0] = returns[index];
        matrices_.IdentityMatrix(context);
    } else if (r[5].SD[0] == 0 || r[6].SD[0] == 0) {
        const bool first = r[5].SD[0] != 0;
        const uint32_t offset = first ? 32 : 16;
        const auto axis = first ? (axes == AxisPair::YZ ? Axis::Yaw : Axis::Pitch) :
            (axes == AxisPair::XY ? Axis::Yaw : Axis::Roll);
        r[1].SD[0] = 0x38c90fdb;
        f[1].UL = 0x38c90fdb;
        if (first) memory_.Write32(stack, r[5].UL[0]);
        f[0].f = AngleRadians(first ? r[5].SL[0] : r[6].SL[0]);
        memory_.Write32(stack + offset, f[0].UL);
        EvaluateScratch(context, offset, false);
        FinishAxisMatrix(context, axis, offset);
    } else {
        r[1].SD[0] = 0x38c90fdb;
        f[1].UL = 0x38c90fdb;
        f[0].f = AngleRadians(r[6].SL[0]);
        memory_.Write32(stack, f[0].UL);
        EvaluateScratch(context);
        const unsigned sine = axes == AxisPair::XY ? 4 : 5;
        const unsigned cosine = axes == AxisPair::XY ? 3 : axes == AxisPair::XZ ? 4 : 2;
        f[sine].UL = memory_.Read32(stack);
        f[cosine].UL = memory_.Read32(stack + 4);
        f[0].f = AngleRadians(r[5].SL[0]);
        memory_.Write32(stack, f[0].UL);
        EvaluateScratch(context, 0, false);
        FinishMatrixPair(context, axes);
    }
    r[31].UD[0] = saved_return;
    MoveStack(context, 64);
}

void TrigMath::FinishMatrixPair(CpuContext& context, AxisPair axes) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t output = r[4].UL[0], stack = r[29].UL[0];
    const auto write = [&](uint32_t offset, unsigned reg) { memory_.Write32(output + offset, f[reg].UL); };
    r[1].SD[0] = 0x3f800000;
    if (axes == AxisPair::XY) {
        memory_.Write32(output + 4, r[0].UL[0]);
        f[1].f = -f[4].f;
        f[2].UL = 0x3f800000;
        f[0].UL = memory_.Read32(output + 4);
        write(8, 1); write(60, 2); write(28, 0); write(0, 3);
        memory_.Write32(output + 12, r[0].UL[0]);
    } else {
        f[0].UL = r[0].UL[0];
        f[1].UL = 0x3f800000;
        if (axes == AxisPair::XZ) {
            write(28, 0); write(60, 1); write(0, 4); write(4, 5); write(12, 0); write(8, 0);
        } else {
            f[3].f = -f[5].f;
            write(60, 1); write(16, 3); write(12, 0); write(20, 2); write(28, 0); write(24, 0);
        }
    }
    write(56, 0); write(52, 0); write(48, 0); write(44, 0);
    for (uint32_t i = 0; i < 4; ++i)
        memory_.Write32((stack & ~15u) + i * 4, context.vuRegs.VF[31].I[i]);
    if (axes == AxisPair::XZ) {
        f[2].UL = memory_.Read32(stack + 4);
        f[0].UL = memory_.Read32(stack);
        f[3].f = -f[2].f;
        write(40, 2);
        f[1].f = -f[0].f;
        write(24, 0);
        f[0].f = Product(f[0].f, f[5].f);
        f[3].f = Product(f[3].f, f[5].f);
        f[1].f = Product(f[1].f, f[4].f);
        f[2].f = Product(f[2].f, f[4].f);
        write(32, 0); write(16, 3); write(36, 1); write(20, 2);
    } else {
        f[0].UL = memory_.Read32(stack + 4);
        f[1].UL = memory_.Read32(stack);
        if (axes == AxisPair::XY) {
            f[2].f = Product(f[0].f, f[3].f);
            write(20, 0);
            f[0].f = Product(f[4].f, f[0].f);
            f[5].f = -f[1].f;
            f[4].f = Product(f[4].f, f[1].f);
            write(40, 2);
            f[1].f = Product(f[1].f, f[3].f);
            write(32, 0); write(36, 5); write(16, 4); write(24, 1);
        } else {
            f[3].f = Product(f[0].f, f[2].f);
            write(40, 0);
            f[4].f = Product(f[1].f, f[5].f);
            f[2].f = Product(f[1].f, f[2].f);
            f[0].f = Product(f[0].f, f[5].f);
            write(0, 3);
            f[1].f = -f[1].f;
            write(36, 4); write(32, 2); write(4, 0); write(8, 1);
        }
    }
}

void TrigMath::MatrixFromPitchYaw(CpuContext& context) { MatrixPair(context, AxisPair::XY); }
void TrigMath::MatrixFromPitchRoll(CpuContext& context) { MatrixPair(context, AxisPair::XZ); }
void TrigMath::MatrixFromYawRoll(CpuContext& context) { MatrixPair(context, AxisPair::YZ); }

void TrigMath::MatrixFromAngles(CpuContext& context) {
    calls_[17].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    const uint64_t saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-80));
    const uint32_t stack = r[29].UL[0];
    memory_.Write32(stack + 64, static_cast<uint32_t>(saved_return));
    memory_.Write32(stack + 68, static_cast<uint32_t>(saved_return >> 32));
    r[5].SD[0] = static_cast<int32_t>(memory_.Read32(r[5].UL[0]));
    if (r[5].SD[0] == r[0].SD[0]) {
        r[2].SD[0] = static_cast<int32_t>(memory_.Read32(r[6].UL[0]));
        r[5].UD[0] = r[29].UD[0] + r[0].UD[0];
        r[3].SD[0] = static_cast<int32_t>(memory_.Read32(r[7].UL[0]));
        r[6].SD[0] = static_cast<int32_t>(stack + 16);
        memory_.Write32(stack, r[2].UL[0]);
        memory_.Write32(stack + 16, r[3].UL[0]);
        r[31].UL[0] = 0x183250;
        MatrixFromYawRoll(context);
    } else {
        r[6].SD[0] = static_cast<int32_t>(memory_.Read32(r[6].UL[0]));
        r[2].SD[0] = static_cast<int32_t>(memory_.Read32(r[7].UL[0]));
        if (r[6].SD[0] == 0) {
            r[6].SD[0] = static_cast<int32_t>(stack + 32);
            memory_.Write32(stack, r[5].UL[0]);
            r[5].UD[0] = r[29].UD[0] + r[0].UD[0];
            memory_.Write32(stack + 32, r[2].UL[0]);
            r[31].UL[0] = 0x183274;
            MatrixFromPitchRoll(context);
        } else if (r[2].SD[0] == 0) {
            memory_.Write32(stack, r[5].UL[0]);
            memory_.Write32(stack + 48, r[6].UL[0]);
            r[5].UD[0] = r[29].UD[0] + r[0].UD[0];
            r[6].SD[0] = static_cast<int32_t>(stack + 48);
            r[31].UL[0] = 0x183298;
            MatrixFromPitchYaw(context);
        } else {
            FinishMatrixAngles(context);
        }
    }
    r[31].UD[0] = saved_return;
    MoveStack(context, 80);
}

void TrigMath::FinishMatrixAngles(CpuContext& context) {
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t stack = r[29].UL[0], output = r[4].UL[0];
    const auto write = [&](uint32_t offset, unsigned reg) { memory_.Write32(output + offset, f[reg].UL); };
    r[1].SD[0] = 0x38c90fdb;
    f[2].UL = 0x38c90fdb;
    f[1].f = AngleRadians(r[2].SL[0]);
    f[0].f = AngleRadians(r[5].SL[0]);
    memory_.Write32(stack + 8, f[1].UL);
    memory_.Write32(stack, f[0].UL);
    EvaluateScratch(context);
    f[9].UL = memory_.Read32(stack);
    f[3].UL = memory_.Read32(stack + 4);
    f[5].UL = memory_.Read32(stack + 12);
    f[0].f = AngleRadians(r[6].SL[0]);
    f[6].UL = memory_.Read32(stack + 8);
    memory_.Write32(stack, f[0].UL);
    EvaluateScratch(context, 0, false);
    f[0].UL = r[0].UL[0];
    r[1].SD[0] = 0x3f800000;
    f[1].UL = 0x3f800000;
    f[13].f = Product(f[3].f, f[5].f);
    f[12].f = Product(f[3].f, f[6].f);
    write(28, 0);
    f[11].f = Product(f[9].f, f[6].f);
    write(60, 1);
    f[10].f = Product(f[9].f, f[5].f);
    write(56, 0); write(52, 0); write(48, 0); write(44, 0); write(12, 0);
    for (uint32_t i = 0; i < 4; ++i)
        memory_.Write32((stack & ~15u) + i * 4, context.vuRegs.VF[31].I[i]);
    f[1].UL = memory_.Read32(stack);
    f[4].UL = memory_.Read32(stack + 4);
    f[2].f = Product(f[1].f, f[12].f);
    f[0].f = Product(f[1].f, f[13].f);
    f[7].f = Product(f[1].f, f[10].f);
    f[8].f = Product(f[1].f, f[11].f);
    f[0].f = Sum(f[0].f, f[11].f);
    f[2].f = Difference(f[2].f, f[10].f);
    f[3].f = Product(f[3].f, f[4].f);
    f[5].f = Product(f[4].f, f[5].f);
    write(32, 0);
    f[6].f = Product(f[4].f, f[6].f);
    write(36, 2);
    f[7].f = Difference(f[7].f, f[12].f);
    write(40, 3);
    f[8].f = Sum(f[8].f, f[13].f);
    write(0, 5);
    f[9].f = Product(f[9].f, f[4].f);
    write(4, 6);
    f[1].f = -f[1].f;
    write(16, 7); write(20, 8); write(24, 9); write(8, 1);
}

void TrigMath::TanOfAngle(CpuContext& context) {
    calls_[13].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const int32_t angle = static_cast<int32_t>(memory_.Read32(r[4].UL[0]));
    r[1].SD[0] = 0x38c90fdb;
    f[1].UL = 0x38c90fdb;
    MoveStack(context, uint32_t(-16));
    f[0].f = AngleRadians(angle);
    memory_.Write32(r[29].UL[0], f[0].UL);
    EvaluateScratch(context);
    f[1].UL = memory_.Read32(r[29].UL[0] + 4);
    f[2].UL = r[0].UL[0];
    auto& flags = context.fpuRegs.fprc[31];
    const bool zero_cosine = f[1].f == f[2].f;
    flags = (flags & ~0x800000u) | (zero_cosine ? 0x800000u : 0);
    f[0].UL = memory_.Read32(r[29].UL[0]);
    if (zero_cosine) {
        const bool positive_sine = f[2].f < f[0].f;
        flags = (flags & ~0x800000u) | (positive_sine ? 0x800000u : 0);
        f[0].UL = positive_sine ? 0x7149f2cau : 0xf149f2cau;
        r[1].SD[0] = static_cast<int32_t>(f[0].UL);
    } else {
        f[0].f = NearestQuotient(f[0].f, f[1].f);
    }
    MoveStack(context, 16);
}

void TrigMath::MatrixAboutAxis(CpuContext& context) {
    calls_[23].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint32_t output = r[4].UL[0];
    const auto write = [&](uint32_t offset, unsigned reg) { memory_.Write32(output + offset, f[reg].UL); };
    f[6].UL = memory_.Read32(r[5].UL[0] + 8);
    f[8].UL = memory_.Read32(r[5].UL[0]);
    r[1].SD[0] = 0x3f800000;
    f[5].UL = 0x3f800000;
    f[7].UL = memory_.Read32(r[5].UL[0] + 4);
    if (r[6].SD[0] == 0) {
        f[0].f = Product(f[8].f, f[8].f);
        r[1].SD[0] = 0x312bcc76;
        f[3].UL = 0x312bcc76;
        f[1].f = Product(f[7].f, f[7].f);
        f[2].f = Product(f[6].f, f[6].f);
        f[0].f = Sum(f[0].f, f[1].f);
        f[0].f = Sum(f[0].f, f[2].f);
        const bool normalizable = f[3].f < f[0].f;
        auto& flags = context.fpuRegs.fprc[31];
        flags = (flags & ~0x800000u) | (normalizable ? 0x800000u : 0);
        if (normalizable) {
            f[0].f = NearestRoot(f[0].f);
            f[0].f = NearestQuotient(f[5].f, f[0].f);
        } else f[0].UL = r[0].UL[0];
        f[5].UL = f[0].UL;
        f[6].f = Product(f[6].f, f[5].f);
        f[8].f = Product(f[8].f, f[5].f);
        f[7].f = Product(f[7].f, f[5].f);
    }
    r[1].SD[0] = 0x3f800000;
    f[0].UL = 0x3f800000;
    memory_.Write32(output + 56, r[0].UL[0]);
    f[5].f = Difference(f[0].f, f[13].f);
    write(60, 0);
    memory_.Write32(output + 52, r[0].UL[0]);
    memory_.Write32(output + 48, r[0].UL[0]);
    f[10].f = Product(f[5].f, f[8].f);
    memory_.Write32(output + 44, r[0].UL[0]);
    f[11].f = Product(f[5].f, f[6].f);
    memory_.Write32(output + 28, r[0].UL[0]);
    f[9].f = Product(f[5].f, f[7].f);
    memory_.Write32(output + 12, r[0].UL[0]);
    f[4].f = Product(f[10].f, f[6].f);
    f[1].f = Product(f[10].f, f[7].f);
    f[5].f = Product(f[9].f, f[6].f);
    f[0].f = Product(f[11].f, f[6].f);
    f[2].f = Product(f[10].f, f[8].f);
    f[3].f = Product(f[9].f, f[7].f);
    f[11].f = Product(f[12].f, f[6].f);
    f[10].f = Product(f[12].f, f[8].f);
    f[9].f = Product(f[12].f, f[7].f);
    f[6].f = Difference(f[1].f, f[11].f);
    f[8].f = Difference(f[5].f, f[10].f);
    f[7].f = Sum(f[4].f, f[9].f);
    f[0].f = Sum(f[0].f, f[13].f);
    write(16, 6);
    f[2].f = Sum(f[2].f, f[13].f);
    write(36, 8);
    f[3].f = Sum(f[3].f, f[13].f);
    write(32, 7);
    f[1].f = Sum(f[1].f, f[11].f);
    write(40, 0);
    f[4].f = Difference(f[4].f, f[9].f);
    write(0, 2);
    f[5].f = Sum(f[5].f, f[10].f);
    write(20, 3); write(4, 1); write(8, 4); write(24, 5);
}

void TrigMath::AxisAngleMatrix(CpuContext& context) {
    calls_[24].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& f = context.fpuRegs.fpr;
    const uint64_t saved_return = r[31].UD[0];
    MoveStack(context, uint32_t(-32));
    const uint32_t stack = r[29].UL[0];
    memory_.Write32(stack + 16, static_cast<uint32_t>(saved_return));
    memory_.Write32(stack + 20, static_cast<uint32_t>(saved_return >> 32));
    r[6].SD[0] = static_cast<int32_t>(memory_.Read32(r[6].UL[0]));
    if (r[6].SD[0] == 0) {
        r[31].UL[0] = 0x18cec4;
        matrices_.IdentityMatrix(context);
    } else {
        f[0].f = AngleRadians(r[6].SL[0]);
        r[1].SD[0] = 0x38c90fdb;
        f[1].UL = 0x38c90fdb;
        memory_.Write32(stack, f[0].UL);
        EvaluateScratch(context);
        f[13].UL = memory_.Read32(stack + 4);
        r[6].SD[0] = r[7].SD[0] + r[0].SD[0];
        f[12].UL = memory_.Read32(stack);
        r[31].UL[0] = 0x18cf18;
        MatrixAboutAxis(context);
    }
    r[31].UD[0] = saved_return;
    MoveStack(context, 32);
}

TrigMath::Counters TrigMath::Counts() const {
    Counters result{};
    for (size_t i = 0; i < calls_.size(); ++i) result.entries[i] = calls_[i].load(std::memory_order_relaxed);
    result.generated_continuations = continuations_->load(std::memory_order_relaxed);
    for (size_t i = 0; i < result.generated_body_calls.size(); ++i)
        result.generated_body_calls[i] = (*generated_body_calls_)[i].load(std::memory_order_relaxed);
    return result;
}

std::string TrigMath::Census() const {
    const auto counts = Counts();
    constexpr const char* names[] = {"cos_sin16", "sin_cos16", "sin_cos16_pair", "pitch", "yaw", "roll",
        "rotation_xy", "rotation_xz", "rotation_yz", "matrix_x", "matrix_y", "matrix_z", "rotation_xyz", "tangent",
        "matrix_xy", "matrix_xz", "matrix_yz", "matrix_xyz", "turn_toward",
        "turn_sine_cosine", "turn_axis", "rotation_axis_sine", "rotation_axis",
        "matrix_axis_sine", "matrix_axis", "inverse_length4", "slerp", "angle_from", "angle_point",
        "euler_angles", "snap_yaw"};
    std::string result;
    for (size_t i = 0; i < calls_.size(); ++i) {
        if (i) result += ',';
        result += std::string(names[i]) + '=' + std::to_string(counts.entries[i]);
    }
    result += ",generated_continuations=" + std::to_string(counts.generated_continuations);
    for (size_t i = 0; i < counts.generated_body_calls.size(); ++i)
        result += std::string(",generated_body_") + names[i] + '=' + std::to_string(counts.generated_body_calls[i]);
    return result;
}

TrigMath::~TrigMath() {
    if (installation_) installation_->store(false, std::memory_order_release);
}

}  // namespace title
