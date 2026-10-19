#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "engine/ee/cpu/cpu_state.h"

namespace ps2r::ipu {
class GuestMemory;
}

namespace title {

// Game math library. Register writes here preserve the generated
// caller ABI; evaluating the math does not execute a VU macro or interpreter.
class MathLibrary {
public:
    using GuestMemory = ps2r::ipu::GuestMemory;
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    using Vector = std::array<float, 4>;
    using Matrix = std::array<Vector, 4>;

    struct TransformPointResult {
        Vector output;
        Vector accumulator;
    };

    struct RotationResult {
        Vector output;
        std::array<float, 3> accumulator;
    };

    struct MatrixProductResult {
        Matrix output;
        Vector accumulator;
    };

    struct RotationProductResult {
        Vector output;
        Vector products;
        Vector accumulator;
    };

    struct RotationMatrixResult {
        std::array<Vector, 3> rows;
        Vector difference;
        Vector sum;
        std::array<float, 3> accumulator;
    };

    struct RigidInverseResult {
        Matrix output;
        std::array<float, 3> accumulator;
    };

    struct TrianglePlaneResult {
        Vector plane;
        Vector edge_products;
        std::array<float, 3> length_components;
        std::array<float, 3> accumulator;
        float reciprocal_length;
        bool valid;
    };

    struct Counters {
        uint64_t transform_point;
        uint64_t rotate_vector;
        uint64_t transform_vector;
        uint64_t multiply_matrices;
        uint64_t identity_matrix;
        uint64_t multiply_rotations;
        uint64_t rotation_matrix;
        uint64_t transpose;
        uint64_t transpose_in_place;
        uint64_t transpose_rotation;
        uint64_t invert_rigid;
        uint64_t invert_rigid_in_place;
        uint64_t triangle_plane;
        uint64_t generated_continuations;
        // Same order as the thirteen operation fields above. Counted at
        // captured-body entry, in both control and replacement selections.
        std::array<uint64_t, 13> generated_body_calls;
    };

    // Complete NTSC entry points; source/address provenance is in the .cpp.
    static constexpr uint32_t kRotateVectorAddress = 0x0018e4b8u;
    static constexpr uint32_t kTransformPointAddress = 0x0018e4f0u;
    static constexpr uint32_t kTransformVectorAddress = 0x0018e530u;
    static constexpr uint32_t kMultiplyMatricesAddress = 0x0018e5a0u;
    static constexpr uint32_t kIdentityMatrixAddress = 0x0018e570u;
    static constexpr uint32_t kMultiplyRotationsAddress = 0x0018d558u;
    static constexpr uint32_t kRotationMatrixAddress = 0x0018e620u;
    static constexpr uint32_t kTransposeAddress = 0x0018e6b0u;
    static constexpr uint32_t kTransposeInPlaceAddress = 0x0018e6f8u;
    static constexpr uint32_t kTransposeRotationAddress = 0x0018e740u;
    static constexpr uint32_t kInvertRigidAddress = 0x0018e788u;
    static constexpr uint32_t kInvertRigidInPlaceAddress = 0x0018e808u;
    static constexpr uint32_t kTrianglePlaneAddress = 0x0018d0a0u;

    explicit MathLibrary(GuestMemory& memory);
    ~MathLibrary();

    // Installs point transformation only unless include_helpers is requested.
    // The extended set stops at the first failure; false may leave earlier
    // entries installed. This object and memory must outlive all callbacks.
    bool Install(const Installer& installer, bool include_helpers = false);

    // Live integration: all thirteen verified complete entries. Capture every
    // original before installing anything; interior entries keep their original
    // continuation, without replaying an HLE prologue. A failed batch restores
    // captured bodies; even if restoration is rejected, its published wrappers
    // stay inactive and forward directly to those originals. Installer callbacks
    // must replace existing entries to support physical rollback. The older
    // overload above retains its point-only / four-helper behavior.
    // replace_helpers=false retains the accepted TransformPoint replacement
    // and installs counted originals for the other twelve. true replaces all
    // thirteen. Capture must return the pristine generated bodies, not a
    // previously installed wrapper. Counters accumulate for this object's
    // lifetime; the noncapturing legacy overload cannot observe originals.
    // Only one successful batch may belong to this object. Destruction must
    // be quiescent; retained capturing callbacks then call only their originals.
    // Control preserves existing replay proof; selected ordinary entries gain
    // none. Counting an interior does not authorize an unproven resume address.
    bool Install(const Capture& capture, const Installer& installer, bool replace_helpers = true);

    // a0 = matrix, a1 = point, a2 = output (guest addresses in low 32 bits).
    // Reads all inputs before writing output, including when they overlap.
    // Preserves all context except VF26..31 and ACC; PC/RA follow the generated
    // function's ordinary C++ return convention.
    void TransformPoint(CpuContext& context);

    // a0 = matrix, a1 = vector, a2 = output. Rotates xyz, retains input w.
    // Changes VF26..29, VF31, and ACC.xyz (partial x/y sum); VF30/ACC.w survive.
    void RotateVector(CpuContext& context);

    // Same ABI as TransformPoint, but the fourth term uses the input vector w.
    void TransformVector(CpuContext& context);

    // a0 = left matrix, a1 = right matrix, a2 = output; output = left * right.
    // Changes VF23..30 and ACC; ACC is the last row's sum before its w term.
    // All input rows are read before stores; output may overlap either input.
    void MultiplyMatrices(CpuContext& context);

    // a0 = output. VF28..30 change; VF0 is the generated constant-row input.
    void IdentityMatrix(CpuContext& context);
    // a0 = output, a1/a2 = rotations. VF1, VF29..31 and ACC change.
    void MultiplyRotations(CpuContext& context);
    // a0 = output, a1 = rotation. Writes three rows, leaves translation intact.
    // Changes low 64 bits of v0, I, VF1/2/28..30, VF3.y and ACC.xyz.
    void RotationMatrix(CpuContext& context);
    // a0 = input, a1 = output, or a0 = both for the in-place entry.
    // Copies raw words, including NaN payloads; changes GPR8..15 only.
    void Transpose(CpuContext& context);
    void TransposeInPlace(CpuContext& context);
    // Reads three input rows and VF0, writes three transposed rows and VF0.
    void TransposeRotation(CpuContext& context);
    // a0 = output, a1 = input, or a0 = both for the in-place entry.
    // Changes GPR8..15, VF28..31 and ACC.xyz; ACC.w is preserved.
    void InvertRigid(CpuContext& context);
    void InvertRigidInPlace(CpuContext& context);
    // a0 = plane output, a1/a2/a3 = vertices. Returns the area threshold test
    // in v0, preserves the retail temporary at sp-16 and its FPU/Q/VF outputs.
    // Interior epilogues are delegated by the capturing Install overload.
    void TrianglePlane(CpuContext& context);

    // Value arguments and result, independent of guest memory/registers.
    // result[i] = ((matrix[0][i]*x + matrix[1][i]*y) + matrix[2][i]*z)
    //             + matrix[3][i]*translation_scale, with ordered float clamps.
    // point.w is ignored. translation_scale is normally the constant 1; the
    // adapter supplies VF0.w to retain the generated ABI even for saved state.
    // accumulator is the partial sum before translation. Caller rounding is
    // retained; this is compatibility with the existing numeric primitive.
    static TransformPointResult EvaluateTransformPoint(
        const Matrix& matrix, const Vector& point, float translation_scale = 1.0f);

    // Only matrix rows 0..2 and their xyz lanes participate. Output.w is copied
    // bit-for-bit; the three accumulator lanes contain the sum before the z term.
    static RotationResult EvaluateRotation(const Matrix& matrix, const Vector& vector);
    static TransformPointResult EvaluateTransformVector(const Matrix& matrix, const Vector& vector);
    static MatrixProductResult EvaluateMatrixProduct(const Matrix& left, const Matrix& right);
    static Matrix EvaluateIdentity();
    static RotationProductResult EvaluateRotationProduct(
        const Vector& a, const Vector& b, float constant_w = 1.0f);
    static RotationMatrixResult EvaluateRotationMatrix(
        const Vector& rotation, const Vector& constant = {0, 0, 0, 1});
    static Matrix EvaluateTranspose(const Matrix& matrix);
    static RigidInverseResult EvaluateRigidInverse(
        const Matrix& matrix, const Vector& constant = {0, 0, 0, 1});
    static TrianglePlaneResult EvaluateTrianglePlane(
        const Vector& first, const Vector& second, const Vector& third,
        const Vector& constant = {0, 0, 0, 1});

    // Retains the original API meaning: point-transform entries only.
    uint64_t CallCount() const;
    Counters Counts() const;
    // Comma-separated per-entry counts. Host observation only; no guest
    // register or savestate layout is extended. generated_continuations counts
    // every delegation from the capturing installer to an original body; it
    // must be zero on a run claiming retirement of these complete boundaries.
    std::string Census() const;

private:
    void TransposeRows(CpuContext& context, uint32_t input, uint32_t output, bool rotation);
    void InvertRows(CpuContext& context, uint32_t input, uint32_t output);
    GuestMemory& memory_;
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint64_t> rotation_calls_{0};
    std::atomic<uint64_t> vector_calls_{0};
    std::atomic<uint64_t> matrix_calls_{0};
    std::atomic<uint64_t> identity_calls_{0};
    std::atomic<uint64_t> rotation_product_calls_{0};
    std::atomic<uint64_t> rotation_matrix_calls_{0};
    std::atomic<uint64_t> transpose_calls_{0};
    std::atomic<uint64_t> transpose_in_place_calls_{0};
    std::atomic<uint64_t> transpose_rotation_calls_{0};
    std::atomic<uint64_t> inverse_calls_{0};
    std::atomic<uint64_t> inverse_in_place_calls_{0};
    std::atomic<uint64_t> plane_calls_{0};
    std::shared_ptr<std::atomic<uint64_t>> continuation_calls_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<bool>> installation_;
    // A rejected rollback may leave inactive callbacks after owner teardown.
    // Their captured original-execution counters must outlive this object.
    std::shared_ptr<std::array<std::atomic<uint64_t>, 13>> generated_body_calls_;
};

}  // namespace title
