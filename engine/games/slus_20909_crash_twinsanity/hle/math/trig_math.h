#pragma once

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

// Source operations from game/math.cpp and game/rotations.cpp. The value API
// evaluates host math; the complete-entry adapters preserve unmigrated callers.
// Numeric compatibility is with NEW's existing libm sine/cosine substitution,
// not the distinct polynomial used inside the animation microprograms.
class TrigMath {
public:
    using GuestMemory = MathLibrary::GuestMemory;
    using Vector = MathLibrary::Vector;
    using Matrix = MathLibrary::Matrix;
    using Body = MathLibrary::Body;
    using Capture = MathLibrary::Capture;
    using Installer = MathLibrary::Installer;
    enum class Axis { Pitch, Yaw, Roll };
    enum class AxisPair { XY, XZ, YZ };
    static constexpr size_t kEntryCount = 31;

    struct Counters {
        std::array<uint64_t, kEntryCount> entries;
        uint64_t generated_continuations;
        std::array<uint64_t, kEntryCount> generated_body_calls;
    };

    static constexpr uint32_t kCosSin16Address = 0x0018b918;
    static constexpr uint32_t kSinCos16Address = 0x002c68f0;
    static constexpr uint32_t kSinCos16PairAddress = 0x002c6988;
    static constexpr uint32_t kPitchAddress = 0x0018d590;
    static constexpr uint32_t kYawAddress = 0x0018d620;
    static constexpr uint32_t kRollAddress = 0x0018d6b0;
    static constexpr uint32_t kRotationXYAddress = 0x001862a8;
    static constexpr uint32_t kRotationXZAddress = 0x00186448;
    static constexpr uint32_t kRotationYZAddress = 0x001865e0;
    static constexpr uint32_t kMatrixXAddress = 0x0018bae8;
    static constexpr uint32_t kMatrixYAddress = 0x0018bbc0;
    static constexpr uint32_t kMatrixZAddress = 0x0018bc98;
    static constexpr uint32_t kRotationXYZAddress = 0x00186780;
    static constexpr uint32_t kTangentAddress = 0x0018b968;
    static constexpr uint32_t kMatrixXYAddress = 0x00182ad0;
    static constexpr uint32_t kMatrixXZAddress = 0x00182d40;
    static constexpr uint32_t kMatrixYZAddress = 0x00182fb0;
    static constexpr uint32_t kMatrixXYZAddress = 0x00183220;
    static constexpr uint32_t kTurnTowardAddress = 0x00188300;
    static constexpr uint32_t kTurnSineCosineAddress = 0x001881c0;
    static constexpr uint32_t kTurnAxisAddress = 0x0018de00;
    static constexpr uint32_t kRotationAxisSineAddress = 0x0018d818;
    static constexpr uint32_t kRotationAxisAddress = 0x0018d798;
    static constexpr uint32_t kMatrixAxisSineAddress = 0x00185058;
    static constexpr uint32_t kMatrixAxisAddress = 0x0018cea8;
    static constexpr uint32_t kInverseLength4Address = 0x0011c0b8;
    static constexpr uint32_t kSlerpAddress = 0x00187100;
    static constexpr uint32_t kAngleFromAddress = 0x0011bfa0;
    static constexpr uint32_t kAngleOfPointAddress = 0x00182918;
    static constexpr uint32_t kEulerAnglesAddress = 0x00185180;
    static constexpr uint32_t kSnapYawAddress = 0x001874b8;

    explicit TrigMath(GuestMemory& memory);
    ~TrigMath();
    // Atomic batch activation/rollback; only full entries are replaced.
    // false counts and executes all pristine originals; true replaces
    // complete entries. Both modes count originals immediately at execution.
    // Capture must return pristine generated bodies, not installed wrappers.
    // One successful batch per owner; teardown requires quiescent dispatch.
    // Retained callbacks forward to originals after destruction.
    bool Install(const Capture& capture, const Installer& installer, bool replace_helpers = true);

    // Returns {sin(first), cos(first), sin(second), cos(second)}. Evaluation
    // uses chop rounding and restores the caller mode. Nonfinite arguments
    // saturate with sign; denormal inputs/outputs preserve sign as zero.
    static Vector EvaluateSinCos(float first, float second);
    // Signed 65536ths of a turn, without masking or unsigned reinterpretation.
    // Conversion/multiplication keep caller rounding, as in the scalar caller.
    static float AngleRadians(int32_t angle);
    static Vector EvaluateRotation(int32_t angle, Axis axis);
    static Vector EvaluateRotationPair(int32_t first, int32_t second, AxisPair axes);
    static Vector EvaluateRotationXYZ(int32_t x, int32_t y, int32_t z);
    static Matrix EvaluateAxisMatrix(int32_t angle, Axis axis);
    // Source finite +/-1e30 sentinel if cosine is zero. The scalar quotient
    // rounds to nearest and then restores the input conversion's caller mode.
    static float EvaluateTangent(int32_t angle);
    static Matrix EvaluateMatrixPair(int32_t first, int32_t second, AxisPair axes);
    static Matrix EvaluateMatrixXYZ(int32_t x, int32_t y, int32_t z);
    // Source direction interpolation uses the game's cubic arc-cosine
    // approximation. Nonparallel output keeps from.w; zero-sine returns to.
    static Vector EvaluateTurnToward(float share, const Vector& from, const Vector& to);
    // Rodrigues' formula. Non-unit axes use the source length threshold and
    // nearest scalar root/division; a degenerate axis leaves the vector intact.
    static Vector EvaluateTurnBySineCosine(float sine, float cosine, const Vector& vector,
                                          const Vector& axis, bool unit_axis);
    static Vector EvaluateTurnAboutAxis(int32_t angle, const Vector& vector,
                                       const Vector& axis, bool unit_axis);
    static Vector EvaluateRotationFromAxisSine(float sine, float cosine, const Vector& axis, bool unit_axis);
    // This source caller truncates the float half-angle to integer turns
    // before converting to radians. Odd +/-1 angles both become integer zero.
    static Vector EvaluateRotationAboutAxis(int32_t angle, const Vector& axis, bool unit_axis);
    static Matrix EvaluateMatrixAboutAxis(float sine, float cosine, const Vector& axis, bool unit_axis);
    static Matrix EvaluateAxisAngleMatrix(int32_t angle, const Vector& axis, bool unit_axis);
    static float EvaluateInverseLength4(float fallback, float epsilon, const Vector& vector);
    // Source quaternion interpolation selects the shorter arc. Its straight
    // path clamps endpoints and normalizes; its curved path extrapolates.
    static Vector EvaluateSlerp(float share, const Vector& from, const Vector& to);
    // Units 1/2 mean degrees/turns; every other unit means radians.
    static int32_t EvaluateAngleFrom(float value, uint32_t unit);
    // Source polynomial and two integer quantization steps, not host atan2.
    static int32_t EvaluateAngleOfPoint(float y, float x);
    static std::array<int32_t, 3> EvaluateEulerAngles(const Matrix& matrix);
    static Vector EvaluateSnapToYaw(const Vector& rotation, float step);

    // Source specs: game/math.cpp:139,158,200; rotations.cpp:482,498,514.
    // NEW shards 009/030 establish NTSC ABI, including ordered stack/output
    // stores and the unused second angle carried through VF20.z/VF31.zw.
    void CosSin16(CpuContext& context);
    void SinCos16(CpuContext& context);
    void SinCos16Pair(CpuContext& context);
    void RotationFromPitch(CpuContext& context);
    void RotationFromYaw(CpuContext& context);
    void RotationFromRoll(CpuContext& context);
    // Source rotations.cpp:530,552,574. Either zero angle selects the source
    // single-axis fast path, retaining its distinct scratch and store order.
    void GetRotationXY(CpuContext& context);
    void GetRotationXZ(CpuContext& context);
    void GetRotationYZ(CpuContext& context);
    // Source rotations.cpp:282,301 and math.cpp:224. Zero angles call the
    // accepted identity operation; nonzero calls preserve ordered matrix and
    // scratch writes, including overlap. a0=matrix, a1=pointer to signed angle.
    void MatrixAboutX(CpuContext& context);
    void MatrixAboutY(CpuContext& context);
    void MatrixRotationZ(CpuContext& context);
    // Source rotations.cpp:596. a0=quaternion, a1/a2/a3=signed angle pointers.
    // Zero-axis branches invoke the proved paired-angle operation; nested
    // helper work is included in its own entry counter as well.
    void GetRotationXYZ(CpuContext& context);
    // Source rotations.cpp:270. a0=angle pointer; FPR0=return value.
    void TanOfAngle(CpuContext& context);
    // Source rotations.cpp:320,354,388. a0=matrix, a1/a2=angle pointers.
    // Both-zero paths reuse identity; single-angle paths keep their distinct
    // scratch offset and all paths retain source output publication order.
    void MatrixFromPitchYaw(CpuContext& context);
    void MatrixFromPitchRoll(CpuContext& context);
    void MatrixFromYawRoll(CpuContext& context);
    // Source rotations.cpp:422. a0=matrix, a1/a2/a3=angle pointers.
    void MatrixFromAngles(CpuContext& context);
    // Source rotations.cpp:163. a0=out,a1=from,a2=to,FPR12=share.
    void TurnToward(CpuContext& context);
    // Source rotations.cpp:125,639. Both update xyz in place, preserving raw w.
    // Leaf: a0=vector,a1=axis,a2=unitAxis,FPR12=sine,FPR13=cosine.
    // Angle caller: a0=vector,a1=axis,a2=angle pointer,a3=unitAxis.
    void TurnBySineCosine(CpuContext& context);
    void TurnAboutAxis(CpuContext& context);
    // Source math.cpp:1294 / rotations.cpp:630. Leaf a0=out,a1=axis,a2=unit,
    // FPR12=sine,FPR13=cosine. Caller a2=angle pointer,a3=unit.
    // Leaf publishes scale before rereading axis, preserving output aliases.
    void RotationFromAxisSine(CpuContext& context);
    void RotationAboutAxis(CpuContext& context);
    // Source math.cpp:1755 / rotations.cpp:469. Leaf a0=matrix,a1=axis,
    // a2=unit,FPR12=sine,FPR13=cosine. Caller a2=angle pointer,a3=unit.
    // A zero angle calls the accepted identity operation without reading axis.
    void MatrixAboutAxis(CpuContext& context);
    void AxisAngleMatrix(CpuContext& context);
    // Source math.cpp:467. a0=vector,FPR12=fallback,FPR13=epsilon,FPR0=result.
    void InverseLength4(CpuContext& context);
    // Source rotations.cpp:198. a0=out,a1=from,a2=to,FPR12=share.
    void SlerpRotations(CpuContext& context);
    // Source math.cpp:163. a0=out,a1=unit,FPR12=value.
    void AngleFrom(CpuContext& context);
    // Source math.cpp:1477. a0=out,FPR12=y,FPR13=x; r2 returns out.
    void AngleOfPoint(CpuContext& context);
    // Source math.cpp:1513. a0=matrix,a1=pitch,a2=yaw,a3=roll.
    void EulerAnglesOfMatrix(CpuContext& context);
    // Source rotations.cpp:61. a0=rotation,FPR12=step radians.
    void SnapToYaw(CpuContext& context);
    Counters Counts() const;
    std::string Census() const;

private:
    static float FromBits(uint32_t bits);
    static uint32_t Bits(float value);
    static float TrigArgument(float value);
    static void MoveStack(CpuContext& context, uint32_t amount);
    static float Product(float first, float second);
    static float Sum(float first, float second);
    static float Difference(float first, float second);
    static float NearestQuotient(float numerator, float denominator);
    static float NearestRoot(float value);
    static float ArcCosine(float cosine);
    static int32_t TruncatedAngle(float value);
    static float PointAngleEstimate(float tangent);
    static Vector ComposeAngles(const Vector& values, AxisPair axes);
    void EvaluateScratch(CpuContext& context, uint32_t offset = 0, bool store = true);
    void Rotation(CpuContext& context, Axis axis);
    void RotationPair(CpuContext& context, AxisPair axes);
    void AxisMatrix(CpuContext& context, Axis axis);
    void FinishAxisMatrix(CpuContext& context, Axis axis, uint32_t scratch_offset);
    void MatrixPair(CpuContext& context, AxisPair axes);
    void FinishMatrixPair(CpuContext& context, AxisPair axes);
    void FinishMatrixAngles(CpuContext& context);
    void FinishTurnToward(CpuContext& context);
    void SlerpCurved(CpuContext& context);
    void FinishSlerpCurved(CpuContext& context);
    void SlerpLinear(CpuContext& context);
    void PointAnglePolynomial(CpuContext& context, bool reciprocal);
    bool EulerYawOnly(CpuContext& context);
    void EulerGeneralYaw(CpuContext& context);
    void EulerOuterAngles(CpuContext& context);
    GuestMemory& memory_;
    MathLibrary matrices_;
    std::array<std::atomic<uint64_t>, kEntryCount> calls_{};
    std::shared_ptr<std::atomic<uint64_t>> continuations_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<bool>> installation_;
    std::shared_ptr<std::array<std::atomic<uint64_t>, kEntryCount>> generated_body_calls_;
};

}  // namespace title
