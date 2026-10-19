#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include <cstring>
#include <memory>
#include <utility>

#include "engine/ee/cpu/ps2_float.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
namespace {

MathLibrary::Vector ReadVector(MathLibrary::GuestMemory& memory, uint32_t address) {
    MathLibrary::Vector vector;
    for (uint32_t lane = 0; lane < 4; ++lane) {
        const uint32_t bits = memory.Read32(address + lane * 4u);
        std::memcpy(&vector[lane], &bits, sizeof(bits));
    }
    return vector;
}

void WriteVector(MathLibrary::GuestMemory& memory, uint32_t address,
                 const MathLibrary::Vector& vector) {
    for (uint32_t lane = 0; lane < 4; ++lane) {
        uint32_t bits;
        std::memcpy(&bits, &vector[lane], sizeof(bits));
        memory.Write32(address + lane * 4u, bits);
    }
}

// Match the existing clamped float operators' individual rounding steps.
// Volatile intermediates prevent an enclosing target's contraction policy from
// turning a multiply followed by an add into a different fused operation.
float Multiply(float a, float b) {
    volatile float result = ps2_float_clamp(a) * ps2_float_clamp(b);
    return ps2_float_clamp(result);
}

float Add(float a, float b) {
    volatile float result = ps2_float_clamp(a) + ps2_float_clamp(b);
    return ps2_float_clamp(result);
}

float Subtract(float a, float b) {
    volatile float result = ps2_float_clamp(a) - ps2_float_clamp(b);
    return ps2_float_clamp(result);
}

// The generated outer-product expressions use scalar arithmetic rather than
// the clamped vector operators. Retain that distinction at these boundaries.
float CrossProductTerm(float a, float b) {
    volatile float result = a * b;
    return result;
}

float CrossProductDifference(float a, float b) {
    volatile float result = a - b;
    return result;
}

MathLibrary::Vector RegisterValue(const VR_reg& reg) {
    MathLibrary::Vector result;
    std::memcpy(result.data(), reg.F, sizeof(reg.F));
    return result;
}

void CopyRegister(VR_reg& reg, const MathLibrary::Vector& vector) {
    std::memcpy(reg.F, vector.data(), sizeof(reg.F));
}

void TransposeRegisters(CpuContext& context, const MathLibrary::Matrix& input,
                        const MathLibrary::Matrix& transposed, bool transpose_fourth) {
    // Publish the scratch temporaries left by the retail packed-word transpose.
    // They remain visible to generated callers but are not used to compute it.
    for (size_t pair = 0; pair < 2; ++pair) {
        for (size_t half = 0; half < 2; ++half) {
            const MathLibrary::Vector words{input[pair * 2][half * 2],
                input[pair * 2 + 1][half * 2], input[pair * 2][half * 2 + 1],
                input[pair * 2 + 1][half * 2 + 1]};
            std::memcpy(context.cpuRegs.GPR.r[12 + pair * 2 + half].UL, words.data(), 16);
        }
    }
    for (size_t row = 0; row < 4; ++row) {
        const auto& value = row == 3 && !transpose_fourth ? input[3] : transposed[row];
        std::memcpy(context.cpuRegs.GPR.r[8 + row].UL, value.data(), 16);
    }
}

}  // namespace

MathLibrary::MathLibrary(GuestMemory& memory)
    : memory_(memory), generated_body_calls_(std::make_shared<std::array<std::atomic<uint64_t>, 13>>()) {}



MathLibrary::TransformPointResult MathLibrary::EvaluateTransformPoint(
    const Matrix& matrix, const Vector& point, float translation_scale) {
    TransformPointResult result;
    for (size_t lane = 0; lane < 4; ++lane) {
        const float x = Multiply(matrix[0][lane], point[0]);
        const float xy = Add(x, Multiply(matrix[1][lane], point[1]));
        result.accumulator[lane] = Add(xy, Multiply(matrix[2][lane], point[2]));
        result.output[lane] =
            Add(result.accumulator[lane], Multiply(matrix[3][lane], translation_scale));
    }
    return result;
}

MathLibrary::RotationResult MathLibrary::EvaluateRotation(
    const Matrix& matrix, const Vector& vector) {
    RotationResult result{vector, {}};
    for (size_t lane = 0; lane < 3; ++lane) {
        result.accumulator[lane] = Add(Multiply(matrix[0][lane], vector[0]),
                                       Multiply(matrix[1][lane], vector[1]));
        result.output[lane] = Add(result.accumulator[lane], Multiply(matrix[2][lane], vector[2]));
    }
    return result;
}

MathLibrary::TransformPointResult MathLibrary::EvaluateTransformVector(
    const Matrix& matrix, const Vector& vector) {
    return EvaluateTransformPoint(matrix, vector, vector[3]);
}

MathLibrary::MatrixProductResult MathLibrary::EvaluateMatrixProduct(
    const Matrix& left, const Matrix& right) {
    MatrixProductResult result;
    for (size_t row = 0; row < 4; ++row) {
        const auto transformed = EvaluateTransformVector(right, left[row]);
        result.output[row] = transformed.output;
        result.accumulator = transformed.accumulator;
    }
    return result;
}

MathLibrary::Matrix MathLibrary::EvaluateIdentity() {
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

MathLibrary::RotationProductResult MathLibrary::EvaluateRotationProduct(
    const Vector& a, const Vector& b, float constant_w) {
    RotationProductResult result;
    for (size_t lane = 0; lane < 4; ++lane)
        result.products[lane] = Multiply(a[lane], b[lane]);
    for (size_t lane = 0; lane < 3; ++lane) {
        const size_t next = (lane + 1) % 3, last = (lane + 2) % 3;
        const float cross = CrossProductTerm(a[next], b[last]);
        result.accumulator[lane] = Add(Add(cross, Multiply(a[lane], b[3])),
                                      Multiply(b[lane], a[3]));
        result.output[lane] = CrossProductDifference(result.accumulator[lane],
                                                     CrossProductTerm(b[next], a[last]));
    }
    result.accumulator[3] = Subtract(Subtract(result.products[3], result.products[2]),
                                     Multiply(constant_w, result.products[1]));
    result.output[3] = Subtract(result.accumulator[3], Multiply(constant_w, result.products[0]));
    return result;
}

MathLibrary::RotationMatrixResult MathLibrary::EvaluateRotationMatrix(
    const Vector& rotation, const Vector& constant) {
    // Source MatrixFromRotation, NTSC 0x18e620: double diagonal squares, but
    // form off-diagonals from sqrt(2)-scaled components. Replacing that order
    // with the algebraically equivalent 2*x*y changes the game's low bits.
    constexpr uint32_t scale_bits = 0x3fb504f3u;
    float scale;
    std::memcpy(&scale, &scale_bits, sizeof(scale));
    Vector doubled_squares{}, scaled{}, ones{};
    for (size_t lane = 0; lane < 4; ++lane)
        scaled[lane] = Multiply(rotation[lane], scale);
    RotationMatrixResult result{};
    result.difference[3] = rotation[3];
    for (size_t lane = 0; lane < 3; ++lane) {
        const float square = Multiply(rotation[lane], rotation[lane]);
        doubled_squares[lane] = Add(square, square);
        ones[lane] = Add(constant[lane], constant[3]);
        const float cross = CrossProductTerm(scaled[(lane + 1) % 3], scaled[(lane + 2) % 3]);
        const float weighted = Multiply(scaled[lane], scaled[3]);
        result.difference[lane] = Subtract(cross, weighted);
        result.sum[lane] = Add(cross, weighted);
    }
    result.sum[3] = result.sum[0];  // The partial rotate copies the completed x.
    result.accumulator = {Subtract(ones[0], Multiply(ones[0], doubled_squares[1])),
                          Subtract(ones[1], Multiply(ones[1], doubled_squares[0])),
                          Subtract(ones[2], Multiply(ones[2], doubled_squares[0]))};
    result.rows[0] = {Subtract(result.accumulator[0], Multiply(ones[0], doubled_squares[2])),
                      result.sum[2], Add(constant[2], result.difference[1]), constant[0]};
    result.rows[1] = {result.difference[2],
                      Subtract(result.accumulator[1], Multiply(ones[1], doubled_squares[2])),
                      result.sum[0], constant[0]};
    result.rows[2] = {result.sum[1], Add(constant[1], result.difference[0]),
                      Subtract(result.accumulator[2], Multiply(ones[2], doubled_squares[1])),
                      constant[0]};
    return result;
}

MathLibrary::Matrix MathLibrary::EvaluateTranspose(const Matrix& matrix) {
    Matrix result;
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            std::memcpy(&result[row][column], &matrix[column][row], sizeof(float));
    return result;
}

MathLibrary::RigidInverseResult MathLibrary::EvaluateRigidInverse(
    const Matrix& matrix, const Vector& constant) {
    RigidInverseResult result{EvaluateTranspose(matrix), {}};
    Vector translation = matrix[3];
    const float sign = Subtract(constant[0], constant[3]);
    for (size_t lane = 0; lane < 3; ++lane) translation[lane] = Multiply(translation[lane], sign);
    std::memcpy(&translation[3], &constant[3], sizeof(float));
    const auto rotated = EvaluateRotation(result.output, translation);
    result.accumulator = rotated.accumulator;
    for (size_t row = 0; row < 3; ++row)
        result.output[row][3] = Subtract(result.output[row][3], result.output[row][3]);
    result.output[3] = rotated.output;
    return result;
}

MathLibrary::TrianglePlaneResult MathLibrary::EvaluateTrianglePlane(
    const Vector& first, const Vector& second, const Vector& third, const Vector& constant) {
    TrianglePlaneResult result{};
    Vector a{}, b{}, cross{};
    for (size_t lane = 0; lane < 4; ++lane) {
        a[lane] = Subtract(second[lane], first[lane]);
        b[lane] = Subtract(third[lane], first[lane]);
    }
    for (size_t lane = 0; lane < 3; ++lane) {
        result.accumulator[lane] = CrossProductTerm(a[(lane + 1) % 3], b[(lane + 2) % 3]);
        cross[lane] = CrossProductDifference(result.accumulator[lane],
            CrossProductTerm(b[(lane + 1) % 3], a[(lane + 2) % 3]));
        result.length_components[lane] = Multiply(cross[lane], cross[lane]);
    }
    const float squared = Add(Add(result.length_components[0], result.length_components[1]),
                               result.length_components[2]);
    // In particular sqrt(MAX_FLOAT) must be evaluated under the caller's
    // rounding mode; optimizing the clamp branch into a precomputed nearest
    // constant moves the normal and the plane by an ulp.
    volatile float root_input = std::fabs(ps2_float_clamp(squared));
    volatile float root = std::sqrt(root_input);
    const float length = Add(constant[0], ps2_float_clamp(root));
    result.length_components[0] = length;
    if (length == 0.0f) {
        uint32_t numerator, denominator;
        std::memcpy(&numerator, &constant[3], sizeof(numerator));
        std::memcpy(&denominator, &length, sizeof(denominator));
        const uint32_t saturated = ((numerator ^ denominator) & 0x80000000u) | 0x7f7fffffu;
        std::memcpy(&result.reciprocal_length, &saturated, sizeof(saturated));
    } else {
        volatile float reciprocal = constant[3] / length;
        result.reciprocal_length = reciprocal;
    }
    result.edge_products[3] = a[3];
    for (size_t lane = 0; lane < 3; ++lane) {
        result.plane[lane] = Multiply(cross[lane], result.reciprocal_length);
        result.edge_products[lane] = Multiply(result.plane[lane], first[lane]);
    }
    result.edge_products[0] = Add(Add(result.edge_products[0], result.edge_products[1]),
                                  result.edge_products[2]);
    result.plane[3] = Subtract(Subtract(constant[3], constant[3]), result.edge_products[0]);
    constexpr uint32_t epsilon_bits = 0x3851b717u;
    float epsilon;
    std::memcpy(&epsilon, &epsilon_bits, sizeof(epsilon));
    result.valid = epsilon < length;
    return result;
}

void MathLibrary::TransformPoint(CpuContext& context) {
    calls_.fetch_add(1, std::memory_order_relaxed);
    // Retain the existing quadword accessors' address alignment at the boundary.
    const uint32_t matrix_address = context.cpuRegs.GPR.r[4].UL[0] & ~0xFu;
    const uint32_t point_address = context.cpuRegs.GPR.r[5].UL[0] & ~0xFu;
    const uint32_t output_address = context.cpuRegs.GPR.r[6].UL[0] & ~0xFu;

    // Source contract: twinsanity-reversed 38d533d0, src/platform/ps2/matrices.cpp
    // VuTransformPoint; PAL/NTSC binding in symbols/pal_ntsc_map.tsv. The NTSC
    // 0x40-byte body loads row 0, point, then rows 1..3 before its sole store.
    Matrix matrix;
    matrix[0] = ReadVector(memory_, matrix_address);
    const Vector point = ReadVector(memory_, point_address);
    for (uint32_t row = 1; row < 4; ++row)
        matrix[row] = ReadVector(memory_, matrix_address + row * 16u);

    const auto result = EvaluateTransformPoint(matrix, point, context.vuRegs.VF[0].w);

    // These are compatibility outputs for generated callers, not inputs to a
    // hardware execution path. Raw input payloads (including point.w) survive.
    for (size_t row = 0; row < 4; ++row)
        CopyRegister(context.vuRegs.VF[27 + row], matrix[row]);
    CopyRegister(context.vuRegs.VF[31], point);
    CopyRegister(context.vuRegs.ACC, result.accumulator);
    CopyRegister(context.vuRegs.VF[26], result.output);
    WriteVector(memory_, output_address, result.output);
}

void MathLibrary::RotateVector(CpuContext& context) {
    rotation_calls_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t matrix_address = context.cpuRegs.GPR.r[4].UL[0] & ~0xFu;
    const uint32_t vector_address = context.cpuRegs.GPR.r[5].UL[0] & ~0xFu;
    const uint32_t output_address = context.cpuRegs.GPR.r[6].UL[0] & ~0xFu;

    // TR 38d533d0 src/platform/ps2/matrices.cpp:7; PAL 0x18ec48 -> NTSC
    // [0x18e4b8,0x18e4f0). The target reads the vector twice; VF26.w is the
    // second load's raw w, while the xyz computation uses the first load.
    Matrix matrix{};
    for (uint32_t row = 0; row < 3; ++row)
        matrix[row] = ReadVector(memory_, matrix_address + row * 16u);
    const Vector vector = ReadVector(memory_, vector_address);
    Vector output = ReadVector(memory_, vector_address);
    const auto result = EvaluateRotation(matrix, vector);
    std::memcpy(output.data(), result.output.data(), sizeof(float) * 3);

    for (size_t row = 0; row < 3; ++row)
        CopyRegister(context.vuRegs.VF[27 + row], matrix[row]);
    CopyRegister(context.vuRegs.VF[31], vector);
    CopyRegister(context.vuRegs.VF[26], output);
    std::memcpy(context.vuRegs.ACC.F, result.accumulator.data(), sizeof(float) * 3);
    WriteVector(memory_, output_address, output);
}

void MathLibrary::TransformVector(CpuContext& context) {
    vector_calls_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t matrix_address = context.cpuRegs.GPR.r[4].UL[0] & ~0xFu;
    const uint32_t vector_address = context.cpuRegs.GPR.r[5].UL[0] & ~0xFu;
    const uint32_t output_address = context.cpuRegs.GPR.r[6].UL[0] & ~0xFu;

    // TR 38d533d0 src/platform/ps2/matrices.cpp:101; PAL 0x18ecc0 -> NTSC
    // [0x18e530,0x18e570). Fourth term is input.w; VF0 does not participate.
    Matrix matrix;
    matrix[0] = ReadVector(memory_, matrix_address);
    const Vector vector = ReadVector(memory_, vector_address);
    for (uint32_t row = 1; row < 4; ++row)
        matrix[row] = ReadVector(memory_, matrix_address + row * 16u);
    const auto result = EvaluateTransformVector(matrix, vector);

    for (size_t row = 0; row < 4; ++row)
        CopyRegister(context.vuRegs.VF[27 + row], matrix[row]);
    CopyRegister(context.vuRegs.VF[31], vector);
    CopyRegister(context.vuRegs.VF[26], result.output);
    CopyRegister(context.vuRegs.ACC, result.accumulator);
    WriteVector(memory_, output_address, result.output);
}

void MathLibrary::MultiplyMatrices(CpuContext& context) {
    matrix_calls_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t left_address = context.cpuRegs.GPR.r[4].UL[0] & ~0xFu;
    const uint32_t right_address = context.cpuRegs.GPR.r[5].UL[0] & ~0xFu;
    const uint32_t output_address = context.cpuRegs.GPR.r[6].UL[0] & ~0xFu;

    // TR 38d533d0 src/platform/ps2/matrices.cpp:139; PAL 0x18ed30 -> NTSC
    // [0x18e5a0,0x18e620). Preserve the target's input order and retain every
    // row before the first output store, including partially overlapping arrays.
    Matrix left, right;
    right[0] = ReadVector(memory_, right_address);
    left[0] = ReadVector(memory_, left_address);
    for (uint32_t row = 1; row < 4; ++row)
        right[row] = ReadVector(memory_, right_address + row * 16u);
    for (uint32_t row = 1; row < 4; ++row)
        left[row] = ReadVector(memory_, left_address + row * 16u);
    const auto result = EvaluateMatrixProduct(left, right);

    for (size_t row = 0; row < 4; ++row) {
        CopyRegister(context.vuRegs.VF[23 + row], result.output[row]);
        CopyRegister(context.vuRegs.VF[27 + row], right[row]);
    }
    CopyRegister(context.vuRegs.ACC, result.accumulator);
    for (uint32_t row = 0; row < 4; ++row)
        WriteVector(memory_, output_address + row * 16u, result.output[row]);
}

void MathLibrary::IdentityMatrix(CpuContext& context) {
    identity_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp InitIdentityMatrix; PAL 0x18ed00 -> NTSC 0x18e570.
    const uint32_t output = context.cpuRegs.GPR.r[4].UL[0] & ~15u;
    const Vector constant = RegisterValue(context.vuRegs.VF[0]);
    WriteVector(memory_, output + 48, constant);
    Vector first = RegisterValue(context.vuRegs.VF[28]);
    Vector second = RegisterValue(context.vuRegs.VF[29]);
    for (size_t lane = 0; lane < 4; ++lane) {
        first[lane] = Subtract(first[lane], first[lane]);
        second[lane] = Subtract(second[lane], second[lane]);
    }
    first[0] = Add(constant[0], constant[3]);
    second[1] = Add(constant[1], constant[3]);
    const Vector third{constant[1], constant[2], constant[3], constant[0]};
    CopyRegister(context.vuRegs.VF[28], first);
    CopyRegister(context.vuRegs.VF[29], second);
    CopyRegister(context.vuRegs.VF[30], third);
    WriteVector(memory_, output + 32, third);
    WriteVector(memory_, output + 16, second);
    WriteVector(memory_, output, first);
}

void MathLibrary::MultiplyRotations(CpuContext& context) {
    rotation_product_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp MultiplyRotations; PAL 0x18dce8 -> NTSC 0x18d558.
    const Vector a = ReadVector(memory_, context.cpuRegs.GPR.r[5].UL[0] & ~15u);
    const Vector b = ReadVector(memory_, context.cpuRegs.GPR.r[6].UL[0] & ~15u);
    const auto result = EvaluateRotationProduct(a, b, context.vuRegs.VF[0].w);
    CopyRegister(context.vuRegs.VF[31], a);
    CopyRegister(context.vuRegs.VF[30], b);
    CopyRegister(context.vuRegs.VF[1], result.products);
    CopyRegister(context.vuRegs.VF[29], result.output);
    CopyRegister(context.vuRegs.ACC, result.accumulator);
    WriteVector(memory_, context.cpuRegs.GPR.r[4].UL[0] & ~15u, result.output);
}

void MathLibrary::RotationMatrix(CpuContext& context) {
    rotation_matrix_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp MatrixFromRotation; PAL 0x18edb0 -> NTSC 0x18e620.
    const Vector rotation = ReadVector(memory_, context.cpuRegs.GPR.r[5].UL[0] & ~15u);
    const auto result = EvaluateRotationMatrix(rotation, RegisterValue(context.vuRegs.VF[0]));
    context.cpuRegs.GPR.r[2].UD[0] = 0x3fb504f3u;
    context.vuRegs.I = 0x3fb504f3u;
    CopyRegister(context.vuRegs.VF[1], result.difference);
    CopyRegister(context.vuRegs.VF[2], result.sum);
    context.vuRegs.VF[3].y = result.difference[2];
    std::memcpy(context.vuRegs.ACC.F, result.accumulator.data(), sizeof(float) * 3);
    for (size_t row = 0; row < 3; ++row) CopyRegister(context.vuRegs.VF[28 + row], result.rows[row]);
    const uint32_t output = context.cpuRegs.GPR.r[4].UL[0] & ~15u;
    for (uint32_t row : {0u, 2u, 1u}) WriteVector(memory_, output + row * 16, result.rows[row]);
}

void MathLibrary::TransposeRows(CpuContext& context, uint32_t input, uint32_t output, bool rotation) {
    Matrix matrix;
    for (uint32_t row = 0; row < (rotation ? 3u : 4u); ++row)
        matrix[row] = ReadVector(memory_, (input & ~15u) + row * 16);
    if (rotation) matrix[3] = RegisterValue(context.vuRegs.VF[0]);
    const Matrix transposed = EvaluateTranspose(matrix);
    TransposeRegisters(context, matrix, transposed, true);
    for (uint32_t row = 0; row < 4; ++row)
        WriteVector(memory_, (output & ~15u) + row * 16,
                    rotation && row == 3 ? matrix[3] : transposed[row]);
}

void MathLibrary::Transpose(CpuContext& context) {
    transpose_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp VuTranspose; PAL 0x18ee40 -> NTSC 0x18e6b0.
    TransposeRows(context, context.cpuRegs.GPR.r[4].UL[0], context.cpuRegs.GPR.r[5].UL[0], false);
}

void MathLibrary::TransposeInPlace(CpuContext& context) {
    transpose_in_place_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp TransposeQuadwords; PAL 0x18ee88 -> NTSC 0x18e6f8.
    TransposeRows(context, context.cpuRegs.GPR.r[4].UL[0], context.cpuRegs.GPR.r[4].UL[0], false);
}

void MathLibrary::TransposeRotation(CpuContext& context) {
    transpose_rotation_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp VuTransposeRotation; PAL 0x18eed0 -> NTSC 0x18e740.
    TransposeRows(context, context.cpuRegs.GPR.r[4].UL[0], context.cpuRegs.GPR.r[5].UL[0], true);
}

void MathLibrary::InvertRows(CpuContext& context, uint32_t input, uint32_t output) {
    Matrix matrix;
    for (uint32_t row = 0; row < 4; ++row)
        matrix[row] = ReadVector(memory_, (input & ~15u) + row * 16);
    const auto result = EvaluateRigidInverse(matrix, RegisterValue(context.vuRegs.VF[0]));
    TransposeRegisters(context, matrix, EvaluateTranspose(matrix), false);
    for (size_t row = 0; row < 4; ++row) CopyRegister(context.vuRegs.VF[28 + row], result.output[row]);
    std::memcpy(context.vuRegs.ACC.F, result.accumulator.data(), sizeof(float) * 3);
    for (uint32_t row : {3u, 0u, 1u, 2u})
        WriteVector(memory_, (output & ~15u) + row * 16, result.output[row]);
}

void MathLibrary::InvertRigid(CpuContext& context) {
    inverse_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp VuInvertRigid; PAL 0x18ef18 -> NTSC 0x18e788.
    InvertRows(context, context.cpuRegs.GPR.r[5].UL[0], context.cpuRegs.GPR.r[4].UL[0]);
}

void MathLibrary::InvertRigidInPlace(CpuContext& context) {
    inverse_in_place_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp VuInvertRigidInPlace; PAL 0x18ef98 -> NTSC 0x18e808.
    // NTSC contains the complete inlined body; it does not call 0x18e788.
    InvertRows(context, context.cpuRegs.GPR.r[4].UL[0], context.cpuRegs.GPR.r[4].UL[0]);
}

void MathLibrary::TrianglePlane(CpuContext& context) {
    plane_calls_.fetch_add(1, std::memory_order_relaxed);
    // TR matrices.cpp PlaneThroughTriangle; PAL 0x18d830 -> NTSC 0x18d0a0.
    // Complete entry only: the 0x18d120 / 0x18d124 epilogues remain captured.
    auto& registers = context.cpuRegs.GPR.r;
    const uint32_t stack = registers[29].UL[0] - 16u;
    registers[29].SD[0] = static_cast<int32_t>(stack);
    const Vector first = ReadVector(memory_, registers[5].UL[0] & ~15u);
    const Vector second = ReadVector(memory_, registers[6].UL[0] & ~15u);
    const Vector third = ReadVector(memory_, registers[7].UL[0] & ~15u);
    const auto result = EvaluateTrianglePlane(first, second, third, RegisterValue(context.vuRegs.VF[0]));
    CopyRegister(context.vuRegs.VF[8], first);
    CopyRegister(context.vuRegs.VF[9], result.edge_products);
    CopyRegister(context.vuRegs.VF[10], result.plane);
    std::memcpy(context.vuRegs.VF[11].F, result.length_components.data(), sizeof(float) * 3);
    std::memcpy(context.vuRegs.ACC.F, result.accumulator.data(), sizeof(float) * 3);
    context.vuRegs.Q = result.reciprocal_length;
    WriteVector(memory_, stack & ~15u, RegisterValue(context.vuRegs.VF[11]));
    WriteVector(memory_, registers[4].UL[0] & ~15u, result.plane);
    registers[1].UD[0] = 0x3851b717u;
    context.fpuRegs.fpr[1].UL = 0x3851b717u;
    // Keep this read after the output store: an overlapping stack/output
    // buffer changes the generated return test but not the computed plane.
    context.fpuRegs.fpr[0].UL = memory_.Read32(stack);
    const bool valid = context.fpuRegs.fpr[1].f < context.fpuRegs.fpr[0].f;
    context.fpuRegs.fprc[31] = (context.fpuRegs.fprc[31] & ~0x800000u) | (valid ? 0x800000u : 0u);
    if (valid) registers[2].SD[0] = static_cast<int32_t>(registers[0].UL[0] + 1u);
    else registers[2].UD[0] = registers[0].UD[0] + registers[0].UD[0];
    registers[29].SD[0] = static_cast<int32_t>(stack + 16u);
}

uint64_t MathLibrary::CallCount() const {
    return calls_.load(std::memory_order_relaxed);
}

MathLibrary::Counters MathLibrary::Counts() const {
    Counters result{CallCount(), rotation_calls_.load(std::memory_order_relaxed),
            vector_calls_.load(std::memory_order_relaxed), matrix_calls_.load(std::memory_order_relaxed),
            identity_calls_.load(std::memory_order_relaxed), rotation_product_calls_.load(std::memory_order_relaxed),
            rotation_matrix_calls_.load(std::memory_order_relaxed), transpose_calls_.load(std::memory_order_relaxed),
            transpose_in_place_calls_.load(std::memory_order_relaxed),
            transpose_rotation_calls_.load(std::memory_order_relaxed), inverse_calls_.load(std::memory_order_relaxed),
            inverse_in_place_calls_.load(std::memory_order_relaxed), plane_calls_.load(std::memory_order_relaxed),
            continuation_calls_->load(std::memory_order_relaxed), {}};
    for (size_t i = 0; i < result.generated_body_calls.size(); ++i)
        result.generated_body_calls[i] = (*generated_body_calls_)[i].load(std::memory_order_relaxed);
    return result;
}

std::string MathLibrary::Census() const {
    const auto counts = Counts();
    std::string result = "transform_point=" + std::to_string(counts.transform_point) +
           ",rotate_vector=" + std::to_string(counts.rotate_vector) +
           ",transform_vector=" + std::to_string(counts.transform_vector) +
           ",multiply_matrices=" + std::to_string(counts.multiply_matrices) +
           ",identity_matrix=" + std::to_string(counts.identity_matrix) +
           ",multiply_rotations=" + std::to_string(counts.multiply_rotations) +
           ",rotation_matrix=" + std::to_string(counts.rotation_matrix) +
           ",transpose=" + std::to_string(counts.transpose) +
           ",transpose_in_place=" + std::to_string(counts.transpose_in_place) +
           ",transpose_rotation=" + std::to_string(counts.transpose_rotation) +
           ",invert_rigid=" + std::to_string(counts.invert_rigid) +
           ",invert_rigid_in_place=" + std::to_string(counts.invert_rigid_in_place) +
           ",triangle_plane=" + std::to_string(counts.triangle_plane) +
           ",generated_continuations=" + std::to_string(counts.generated_continuations);
    constexpr const char* names[] = {"transform_point", "rotate_vector", "transform_vector", "multiply_matrices",
        "identity_matrix", "multiply_rotations", "rotation_matrix", "transpose", "transpose_in_place",
        "transpose_rotation", "invert_rigid", "invert_rigid_in_place", "triangle_plane"};
    for (size_t i = 0; i < counts.generated_body_calls.size(); ++i)
        result += std::string(",generated_body_") + names[i] + '=' + std::to_string(counts.generated_body_calls[i]);
    return result;
}

MathLibrary::~MathLibrary() {
    if (installation_) installation_->store(false, std::memory_order_release);
}

}  // namespace title
