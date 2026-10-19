// Standalone math library contract, with real dispatch wrappers and fixture
// storage. No generated function, VU unit, macro bridge or interpreter is linked.
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "engine/ipu/hle/guest_memory.h"

namespace {

using Math = title::MathLibrary;
using Vector = Math::Vector;
using Matrix = Math::Matrix;

const std::string extended_zero_counts =
    ",identity_matrix=0,multiply_rotations=0,rotation_matrix=0,transpose=0,"
    "transpose_in_place=0,transpose_rotation=0,invert_rigid=0,invert_rigid_in_place=0,triangle_plane=0,"
    "generated_continuations=0,generated_body_transform_point=0,generated_body_rotate_vector=0,"
    "generated_body_transform_vector=0,generated_body_multiply_matrices=0,generated_body_identity_matrix=0,"
    "generated_body_multiply_rotations=0,generated_body_rotation_matrix=0,generated_body_transpose=0,"
    "generated_body_transpose_in_place=0,generated_body_transpose_rotation=0,generated_body_invert_rigid=0,"
    "generated_body_invert_rigid_in_place=0,generated_body_triangle_plane=0";

int failures = 0;
int checks = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(condition) Check((condition), #condition, __LINE__)

float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void ExpectBits(const Vector& actual, const std::array<uint32_t, 4>& expected) {
    for (size_t lane = 0; lane < 4; ++lane) {
        CHECK(Bits(actual[lane]) == expected[lane]);
        if (Bits(actual[lane]) != expected[lane])
            std::printf("  lane %zu: %08x != %08x\n",
                        lane, Bits(actual[lane]), expected[lane]);
    }
}

void ExpectVector(const Vector& actual, const Vector& expected) {
    ExpectBits(actual, {Bits(expected[0]), Bits(expected[1]),
                        Bits(expected[2]), Bits(expected[3])});
}

const Matrix matrix{{{2, 3, 5, 7}, {11, 13, 17, 19},
                     {23, 29, 31, 37}, {41, 43, 47, 53}}};
const Vector accumulator{63, 83, 83, 105};
const Vector output{104, 126, 130, 158};

class TestMathLibrary final : public Math::GuestMemory {
public:
    using Bytes = std::array<uint8_t, 0x800>;
    Bytes bytes{};
    std::vector<uint32_t> reads;
    std::vector<uint32_t> writes;
    bool read_after_write = false;

    uint8_t* Translate(uint32_t address, size_t length) override {
        if (address > bytes.size() || length > bytes.size() - address) std::abort();
        return bytes.data() + address;
    }

    uint32_t Read32(uint32_t address) override {
        read_after_write |= !writes.empty();
        reads.push_back(address);
        const auto* p = Translate(address, 4);
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
               (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    void Write32(uint32_t address, uint32_t value) override {
        writes.push_back(address);
        auto* p = Translate(address, 4);
        for (size_t byte = 0; byte < 4; ++byte)
            p[byte] = static_cast<uint8_t>(value >> (8 * byte));
    }

    static void PutVector(Bytes& target, uint32_t address, const Vector& vector) {
        for (size_t lane = 0; lane < 4; ++lane) {
            const uint32_t bits = Bits(vector[lane]);
            for (size_t byte = 0; byte < 4; ++byte)
                target.at(address + lane * 4 + byte) =
                    static_cast<uint8_t>(bits >> (8 * byte));
        }
    }

    void Seed(const Matrix& input_matrix, const Vector& point) {
        bytes.fill(0x5a);
        for (size_t row = 0; row < 4; ++row)
            PutVector(bytes, 0x100 + row * 16, input_matrix[row]);
        PutVector(bytes, 0x200, point);
    }
};

void TestValueArguments() {
    const Vector point{2, -3, 4, Float(0x7fc12345)};
    const Matrix identity{{{1, 0, 0, 0}, {0, 1, 0, 0},
                           {0, 0, 1, 0}, {0, 0, 0, 1}}};
    const auto simple = Math::EvaluateTransformPoint(identity, point);
    ExpectVector(simple.output, {2, -3, 4, 1});
    ExpectVector(simple.accumulator, {2, -3, 4, 0});

    const auto transformed = Math::EvaluateTransformPoint(matrix, point);
    ExpectVector(transformed.output, output);
    ExpectVector(transformed.accumulator, accumulator);
    const Vector different_w{2, -3, 4, -9876};
    ExpectVector(Math::EvaluateTransformPoint(matrix, different_w).output, output);
    ExpectVector(Math::EvaluateTransformPoint(matrix, point, 2).output,
                 {145, 169, 177, 211});

    // Assignment back into an argument is safe: the pure helper returns a value.
    auto in_place = point;
    in_place = Math::EvaluateTransformPoint(matrix, in_place).output;
    ExpectVector(in_place, output);
    auto matrix_alias = matrix;
    matrix_alias[0] = Math::EvaluateTransformPoint(matrix_alias, point).output;
    ExpectVector(matrix_alias[0], output);
    CHECK(Bits(point[3]) == 0x7fc12345u);
}

void TestNumericEdges() {
    Matrix special{};
    special[0] = {Float(0x7f800000), Float(0x7fc12345),
                  Float(0xff800000), Float(0xffc54321)};
    ExpectBits(Math::EvaluateTransformPoint(special, {1, 0, 0, 0}).output,
               {0x7f7fffff, 0x7f7fffff, 0xff7fffff, 0xff7fffff});
    // Clamping happens before multiplication, so non-finite * zero is zero.
    ExpectBits(Math::EvaluateTransformPoint(special, {0, 0, 0, 0}).output,
               {0, 0, 0, 0});

    const float maximum = Float(0x7f7fffff);
    special[0] = {maximum, -maximum, 16777216, 16777216};
    special[1] = {maximum, -maximum, 1, -16777216};
    special[2] = {0, 0, -16777216, 1};
    ExpectVector(Math::EvaluateTransformPoint(special, {1, 1, 1, 0}).output,
                 {maximum, -maximum, 0, 1});
    // Saturate each product before cancellation; clamping only the final sum
    // would produce a different result for the first two lanes.
    special = {};
    special[0] = {maximum, -maximum, 0, 0};
    special[1] = {-maximum, maximum, 0, 0};
    ExpectVector(Math::EvaluateTransformPoint(special, {2, 1, 0, 0}).output,
                 {0, 0, 0, 0});

    special = {};
    special[0] = {Float(1), Float(0x80000001), Float(0x007fffff), 0};
    ExpectBits(Math::EvaluateTransformPoint(special, {1, 0, 0, 0}).output,
               {1, 0x80000001, 0x007fffff, 0});
    for (auto& row : special) row.fill(Float(0x80000000));
    ExpectBits(Math::EvaluateTransformPoint(special, {1, 1, 1, 0}).output,
               {0x80000000, 0x80000000, 0x80000000, 0x80000000});
}

void TestSeparateRounding() {
    Matrix cancellation{};
    cancellation[0][0] = -1;
    cancellation[1][0] = Float(0x3f800001);  // 1 + 2^-23
    const Vector point{1, Float(0x3f7ffffe), 0, 0};  // 1 - 2^-23
    CHECK(std::fesetround(FE_TONEAREST) == 0);
    // The rounded product is 1. A fused multiply-add would instead be -2^-46.
    ExpectBits(Math::EvaluateTransformPoint(cancellation, point).output, {0, 0, 0, 0});
    CHECK(std::fegetround() == FE_TONEAREST);
    CHECK(std::fesetround(FE_TOWARDZERO) == 0);
    // With the caller's rounding toward zero, the product is 1 - 2^-24.
    ExpectBits(Math::EvaluateTransformPoint(cancellation, point).output,
               {0xb3800000, 0, 0, 0});
    CHECK(std::fegetround() == FE_TOWARDZERO);
    CHECK(std::fesetround(FE_TONEAREST) == 0);
}

void SetRegister(VR_reg& reg, const Vector& value) {
    std::memcpy(reg.F, value.data(), sizeof(reg.F));
}

std::map<uint32_t, Math::Body> CaptureBindings(Math& math) {
    std::map<uint32_t, Math::Body> installed;
    CHECK(math.Install([&](uint32_t address, Math::Body body) {
        CHECK(bool(body));
        return installed.emplace(address, std::move(body)).second;
    }, true));
    // All four verified entries must be installed exactly once. The original
    // point-transform fixture still invokes and checks its own complete ABI.
    CHECK(installed.size() == 4);
    CHECK(installed.count(0x0018e4b8u) == 1);
    CHECK(installed.count(0x0018e4f0u) == 1);
    CHECK(installed.count(0x0018e530u) == 1);
    CHECK(installed.count(0x0018e5a0u) == 1);
    return installed;
}

void TestAdapter(uint32_t destination, bool low_address_bits, float translation_scale) {
    TestMathLibrary memory;
    const Vector point{2, -3, 4, Float(0xffc12345)};
    memory.Seed(matrix, point);
    Math math(memory);
    const auto installed = CaptureBindings(math);
    CHECK(math.CallCount() == 0);
    CHECK(math.Census() == "transform_point=0,rotate_vector=0,transform_vector=0,multiply_matrices=0" +
                           extended_zero_counts);

    CpuContext context;
    std::memset(&context, 0xa5, sizeof(context));
    context.vuRegs.VF[0].w = translation_scale;
    // Nonzero upper GPR bits must not become part of guest addresses.
    context.cpuRegs.GPR.r[4].UL[0] = 0x100 | (low_address_bits ? 3 : 0);
    context.cpuRegs.GPR.r[5].UL[0] = 0x200 | (low_address_bits ? 7 : 0);
    context.cpuRegs.GPR.r[6].UL[0] = destination | (low_address_bits ? 15 : 0);
    CpuContext expected;
    std::memcpy(&expected, &context, sizeof(expected));
    const Vector expected_output = translation_scale == 1 ? output : Vector{145, 169, 177, 211};
    for (size_t row = 0; row < 4; ++row)
        SetRegister(expected.vuRegs.VF[27 + row], matrix[row]);
    SetRegister(expected.vuRegs.VF[31], point);
    SetRegister(expected.vuRegs.VF[26], expected_output);
    SetRegister(expected.vuRegs.ACC, accumulator);
    auto expected_bytes = memory.bytes;
    TestMathLibrary::PutVector(expected_bytes, destination, expected_output);

    installed.at(Math::kTransformPointAddress)(context, Math::kTransformPointAddress);
    CHECK(math.CallCount() == 1);
    CHECK(math.Census() == "transform_point=1,rotate_vector=0,transform_vector=0,multiply_matrices=0" +
                           extended_zero_counts);
    CHECK(memory.bytes == expected_bytes);
    // Includes GPRs, PC/RA, FPU, VF0..25, VI, Q/P/I and flags, plus untouched
    // bytes/padding: only the six loaded/result vectors and ACC may change.
    CHECK(std::memcmp(&context, &expected, sizeof(context)) == 0);
    CHECK(!memory.read_after_write);
    const std::vector<uint32_t> expected_reads{
        0x100, 0x104, 0x108, 0x10c, 0x200, 0x204, 0x208, 0x20c,
        0x110, 0x114, 0x118, 0x11c, 0x120, 0x124, 0x128, 0x12c,
        0x130, 0x134, 0x138, 0x13c};
    CHECK(memory.reads == expected_reads);
    const std::vector<uint32_t> expected_writes{
        destination, destination + 4, destination + 8, destination + 12};
    CHECK(memory.writes == expected_writes);
}

void TestRawInputsAndCounter() {
    TestMathLibrary memory;
    Matrix special{};
    special[0] = {Float(0x7fc12345), Float(0xffc12345),
                  Float(0x7f800000), Float(0x80000001)};
    const Vector point{1, 0, 0, Float(0xffc54321)};
    memory.Seed(special, point);
    CpuContext context{};
    context.vuRegs.VF[0].w = 1;
    context.cpuRegs.GPR.r[4].UL[0] = 0x100;
    context.cpuRegs.GPR.r[5].UL[0] = 0x200;
    context.cpuRegs.GPR.r[6].UL[0] = 0x300;
    Math math(memory);
    math.TransformPoint(context);
    CHECK(std::memcmp(context.vuRegs.VF[27].F, special[0].data(), 16) == 0);
    CHECK(std::memcmp(context.vuRegs.VF[31].F, point.data(), 16) == 0);
    CHECK(context.vuRegs.VF[26].I[0] == 0x7f7fffffu);
    CHECK(context.vuRegs.VF[26].I[1] == 0xff7fffffu);
    CHECK(context.vuRegs.VF[26].I[2] == 0x7f7fffffu);
    CHECK(context.vuRegs.VF[26].I[3] == 0x80000001u);
    for (unsigned i = 0; i < 10; ++i) math.TransformPoint(context);
    CHECK(math.CallCount() == 11);
    CHECK(math.Census() == "transform_point=11,rotate_vector=0,transform_vector=0,multiply_matrices=0" +
                           extended_zero_counts);
}

void TestAdditionalValueArguments() {
    const Vector vector{2, -3, 4, Float(0xffc12345)};
    const auto rotated = Math::EvaluateRotation(matrix, vector);
    ExpectVector(rotated.output, {63, 83, 83, vector[3]});
    CHECK(rotated.accumulator == (std::array<float, 3>{-29, -33, -41}));
    const Vector weighted{2, -3, 4, 2};
    const auto transformed = Math::EvaluateTransformVector(matrix, weighted);
    ExpectVector(transformed.output, {145, 169, 177, 211});
    ExpectVector(transformed.accumulator, accumulator);
    ExpectVector(Math::EvaluateTransformVector(matrix, {2, -3, 4, 0}).output, accumulator);

    const Matrix left{{weighted, {1, 0, 0, 0}, {0, 1, 0, 0}, {1, 2, 3, 4}}};
    const Matrix expected{{{145, 169, 177, 211}, {2, 3, 5, 7},
                           {11, 13, 17, 19}, {257, 288, 320, 368}}};
    const auto product = Math::EvaluateMatrixProduct(left, matrix);
    for (size_t row = 0; row < 4; ++row) ExpectVector(product.output[row], expected[row]);
    ExpectVector(product.accumulator, {93, 116, 132, 156});
    CHECK(Math::EvaluateMatrixProduct(matrix, left).output != product.output);
    auto left_alias = left;
    left_alias = Math::EvaluateMatrixProduct(left_alias, matrix).output;
    CHECK(left_alias == expected);
    auto right_alias = matrix;
    right_alias = Math::EvaluateMatrixProduct(left, right_alias).output;
    CHECK(right_alias == expected);
}

void TestAdditionalNumericEdges() {
    Matrix special{};
    const float maximum = Float(0x7f7fffff);
    special[0] = {maximum, -maximum, Float(1), Float(0x7fc12345)};
    special[1] = {-maximum, maximum, 0, Float(0xffc12345)};
    const Vector weights{2, 1, 0, Float(0x80000000)};
    ExpectBits(Math::EvaluateRotation(special, weights).output, {0, 0, 2, 0x80000000});
    const Matrix left{{weights, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}};
    ExpectBits(Math::EvaluateMatrixProduct(left, special).output[0], {0, 0, 2, 0});

    special = {};
    special[3] = {Float(0x7f800000), Float(0xffc12345), 1, Float(1)};
    ExpectBits(Math::EvaluateTransformVector(special, {0, 0, 0, 1}).output,
               {0x7f7fffff, 0xff7fffff, 0x3f800000, 1});

    Matrix cancellation{};
    cancellation[0][0] = -1;
    cancellation[1][0] = Float(0x3f800001);
    const Vector vector{1, Float(0x3f7ffffe), 0, 0};
    const Matrix rows{{vector, vector, vector, vector}};
    for (const int rounding : {FE_TONEAREST, FE_TOWARDZERO}) {
        CHECK(std::fesetround(rounding) == 0);
        const uint32_t expected = rounding == FE_TONEAREST ? 0u : 0xb3800000u;
        ExpectBits(Math::EvaluateRotation(cancellation, vector).output, {expected, 0, 0, 0});
        ExpectBits(Math::EvaluateTransformVector(cancellation, vector).output, {expected, 0, 0, 0});
        const auto product = Math::EvaluateMatrixProduct(rows, cancellation);
        for (const auto& row : product.output) ExpectBits(row, {expected, 0, 0, 0});
        CHECK(std::fegetround() == rounding);
    }
    CHECK(std::fesetround(FE_TONEAREST) == 0);
}

void AppendVectorAddresses(std::vector<uint32_t>& addresses, uint32_t base) {
    for (uint32_t lane = 0; lane < 4; ++lane) addresses.push_back(base + lane * 4);
}

void TestVectorAdapter(uint32_t destination, bool rotation) {
    TestMathLibrary memory;
    const Vector vector{2, -3, 4, rotation ? Float(0xffc54321) : 2};
    memory.Seed(matrix, vector);
    Math math(memory);
    const auto installed = CaptureBindings(math);
    CpuContext context;
    std::memset(&context, 0xa5, sizeof(context));
    context.cpuRegs.GPR.r[4].UL[0] = 0x10f;
    context.cpuRegs.GPR.r[5].UL[0] = 0x203;
    context.cpuRegs.GPR.r[6].UL[0] = destination | 7;
    context.vuRegs.VF[0].w = -100;  // Neither helper uses the point translation factor.
    context.vuRegs.ACC.I[3] = 0xffc67890;
    CpuContext expected;
    std::memcpy(&expected, &context, sizeof(expected));
    const Vector expected_output = rotation ? Vector{63, 83, 83, vector[3]} :
                                              Vector{145, 169, 177, 211};
    for (size_t row = 0; row < (rotation ? 3u : 4u); ++row)
        SetRegister(expected.vuRegs.VF[27 + row], matrix[row]);
    SetRegister(expected.vuRegs.VF[31], vector);
    SetRegister(expected.vuRegs.VF[26], expected_output);
    if (rotation) {
        const Vector partial{-29, -33, -41, 0};
        std::memcpy(expected.vuRegs.ACC.F, partial.data(), sizeof(float) * 3);
    } else {
        SetRegister(expected.vuRegs.ACC, accumulator);
    }
    auto expected_bytes = memory.bytes;
    TestMathLibrary::PutVector(expected_bytes, destination, expected_output);
    const uint32_t entry = rotation ? Math::kRotateVectorAddress : Math::kTransformVectorAddress;
    installed.at(entry)(context, entry);
    CHECK(memory.bytes == expected_bytes);
    CHECK(std::memcmp(&context, &expected, sizeof(context)) == 0);
    CHECK(!memory.read_after_write);
    std::vector<uint32_t> reads;
    if (rotation) {
        for (uint32_t row = 0; row < 3; ++row) AppendVectorAddresses(reads, 0x100 + row * 16);
        AppendVectorAddresses(reads, 0x200);
        AppendVectorAddresses(reads, 0x200);
    } else {
        AppendVectorAddresses(reads, 0x100);
        AppendVectorAddresses(reads, 0x200);
        for (uint32_t row = 1; row < 4; ++row) AppendVectorAddresses(reads, 0x100 + row * 16);
    }
    CHECK(memory.reads == reads);
    std::vector<uint32_t> writes;
    AppendVectorAddresses(writes, destination);
    CHECK(memory.writes == writes);
    const auto counts = math.Counts();
    CHECK(counts.transform_point == 0);
    CHECK(counts.rotate_vector == (rotation ? 1u : 0u));
    CHECK(counts.transform_vector == (rotation ? 0u : 1u));
    CHECK(counts.multiply_matrices == 0);
    CHECK(math.CallCount() == 0);
}

// Independent exact arithmetic for the small integer matrix/alias fixtures.
// Deliberately computes complete dot products in double; rounding/saturation
// cases have separate bit-pattern goldens above.
Matrix ExactIntegerProduct(const Matrix& left, const Matrix& right) {
    Matrix product{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            double sum = 0;
            for (size_t term = 0; term < 4; ++term)
                sum += double(left[row][term]) * double(right[term][column]);
            product[row][column] = static_cast<float>(sum);
        }
    }
    return product;
}

void TestMatrixAdapter(uint32_t destination, bool same_input) {
    TestMathLibrary memory;
    const Matrix left{{{2, -3, 4, 2}, {1, 0, 0, 0}, {0, 1, 0, 0}, {1, 2, 3, 4}}};
    const Matrix& right = same_input ? left : matrix;
    const uint32_t right_address = same_input ? 0x100 : 0x200;
    memory.Seed(left, right[0]);
    for (size_t row = 1; row < 4; ++row)
        TestMathLibrary::PutVector(memory.bytes, right_address + row * 16, right[row]);
    Math math(memory);
    const auto installed = CaptureBindings(math);
    CpuContext context;
    std::memset(&context, 0xa5, sizeof(context));
    context.cpuRegs.GPR.r[4].UL[0] = 0x10f;
    context.cpuRegs.GPR.r[5].UL[0] = right_address | 3;
    context.cpuRegs.GPR.r[6].UL[0] = destination | 7;
    CpuContext expected;
    std::memcpy(&expected, &context, sizeof(expected));
    const auto product = ExactIntegerProduct(left, right);
    Vector partial{};
    for (size_t lane = 0; lane < 4; ++lane)
        partial[lane] = static_cast<float>(double(left[3][0]) * right[0][lane] +
                                          double(left[3][1]) * right[1][lane] +
                                          double(left[3][2]) * right[2][lane]);
    auto expected_bytes = memory.bytes;
    for (size_t row = 0; row < 4; ++row) {
        TestMathLibrary::PutVector(expected_bytes, destination + row * 16, product[row]);
        SetRegister(expected.vuRegs.VF[23 + row], product[row]);
        SetRegister(expected.vuRegs.VF[27 + row], right[row]);
    }
    SetRegister(expected.vuRegs.ACC, partial);
    installed.at(Math::kMultiplyMatricesAddress)(context, Math::kMultiplyMatricesAddress);
    CHECK(memory.bytes == expected_bytes);
    CHECK(std::memcmp(&context, &expected, sizeof(context)) == 0);
    CHECK(!memory.read_after_write);
    std::vector<uint32_t> reads;
    AppendVectorAddresses(reads, right_address);
    AppendVectorAddresses(reads, 0x100);
    for (uint32_t row = 1; row < 4; ++row) AppendVectorAddresses(reads, right_address + row * 16);
    for (uint32_t row = 1; row < 4; ++row) AppendVectorAddresses(reads, 0x100 + row * 16);
    CHECK(memory.reads == reads);
    std::vector<uint32_t> writes;
    for (uint32_t row = 0; row < 4; ++row) AppendVectorAddresses(writes, destination + row * 16);
    CHECK(memory.writes == writes);
    const auto counts = math.Counts();
    CHECK(counts.transform_point == 0);
    CHECK(counts.rotate_vector == 0);
    CHECK(counts.transform_vector == 0);
    CHECK(counts.multiply_matrices == 1);
    CHECK(math.Census() == "transform_point=0,rotate_vector=0,transform_vector=0,multiply_matrices=1" +
                           extended_zero_counts);
}

void TestInstallFailure() {
    TestMathLibrary memory;
    Math math(memory);
    CHECK(!math.Install({}));
    unsigned requests = 0;
    CHECK(!math.Install([&](uint32_t address, Math::Body body) {
        ++requests;
        CHECK(address == Math::kTransformPointAddress);
        CHECK(bool(body));
        return false;
    }));
    CHECK(requests == 1);
    CHECK(math.CallCount() == 0);
    CHECK(memory.reads.empty());
    CHECK(memory.writes.empty());

    // A later rejected entry reports failure after any prior successful binds;
    // no subsequent entry is installed and no guest operation is executed.
    requests = 0;
    CHECK(!math.Install([&](uint32_t address, Math::Body) {
        ++requests;
        return address != Math::kTransformVectorAddress;
    }, true));
    CHECK(requests == 3);
    CHECK(math.Census() == "transform_point=0,rotate_vector=0,transform_vector=0,multiply_matrices=0" +
                           extended_zero_counts);
    CHECK(memory.reads.empty());
    CHECK(memory.writes.empty());
}

void TestDefaultInstall() {
    TestMathLibrary memory;
    Math math(memory);
    std::vector<uint32_t> entries;
    CHECK(math.Install([&](uint32_t address, Math::Body body) {
        CHECK(bool(body));
        entries.push_back(address);
        return true;
    }));
    CHECK(entries == std::vector<uint32_t>{Math::kTransformPointAddress});
    CHECK(memory.reads.empty());
    CHECK(memory.writes.empty());
}

void TestCompleteInstall() {
    TestMathLibrary memory;
    Math math(memory);
    std::map<uint32_t, Math::Body> installed;
    std::vector<std::pair<uint32_t, uint32_t>> continuations;
    std::vector<uint32_t> captures;
    const Math::Capture capture = [&](uint32_t address) -> Math::Body {
        captures.push_back(address);
        return [&, address](CpuContext&, uint32_t entry) {
            uint64_t entered = 0;
            for (const auto count : math.Counts().generated_body_calls) entered += count;
            CHECK(entered == continuations.size() + 1);  // Increment precedes actual body entry.
            continuations.emplace_back(address, entry);
        };
    };
    const Math::Installer install = [&](uint32_t address, Math::Body body) {
        CHECK(captures.size() == 13);  // Preflight before any dispatch mutation.
        return installed.emplace(address, std::move(body)).second;
    };
    CHECK(!math.Install(Math::Capture{}, install));
    CHECK(!math.Install(capture, Math::Installer{}));
    CHECK(installed.empty());
    CHECK(!math.Install([&](uint32_t address) -> Math::Body {
        return address == Math::kRotationMatrixAddress ? Math::Body{} : capture(address);
    }, install));
    CHECK(installed.empty());
    captures.clear();
    CHECK(math.Install(capture, install));
    CHECK(installed.size() == 13);
    for (const auto& [address, body] : installed) {
        CpuContext context;
        std::memset(&context, 0x96, sizeof(context));
        CpuContext before;
        std::memcpy(&before, &context, sizeof(context));
        // No complete-body replay on an interior continuation, even when
        // address arguments are deliberately unusable.
        body(context, address + 4);
        CHECK(std::memcmp(&context, &before, sizeof(context)) == 0);
        CHECK(continuations.back() == std::make_pair(address, address + 4));
    }
    CHECK(continuations.size() == 13);
    CpuContext epilogue{};
    installed.at(Math::kTrianglePlaneAddress)(epilogue, 0x18d120);
    installed.at(Math::kTrianglePlaneAddress)(epilogue, 0x18d124);
    CHECK(continuations[13] == std::make_pair(Math::kTrianglePlaneAddress, 0x18d120u));
    CHECK(continuations[14] == std::make_pair(Math::kTrianglePlaneAddress, 0x18d124u));
    CHECK(memory.reads.empty());
    CHECK(memory.writes.empty());
    CHECK(math.Counts().generated_continuations == 15);
    for (size_t i = 0; i < 13; ++i) CHECK(math.Counts().generated_body_calls[i] == (i == 12 ? 3 : 1));
    CHECK(math.Counts().transform_point == 0);
    CHECK(math.Counts().triangle_plane == 0);
    CpuContext context{};
    context.vuRegs.VF[0].w = 1;
    context.cpuRegs.GPR.r[4].UL[0] = 0x100;
    installed.at(Math::kIdentityMatrixAddress)(context, 0);
    CHECK(math.Counts().identity_matrix == 1);
    CHECK(continuations.size() == 15);
    unsigned requests = 0;
    CHECK(!math.Install(capture, [&](uint32_t address, Math::Body) {
        ++requests;
        return address != Math::kRotationMatrixAddress;
    }));
    CHECK(requests == 0);  // A live owner cannot install a second callback batch.
}

void TestInstallRollback() {
    // Each failure position, including a rejecting installer that has already
    // published the callback. Restored dispatch must call the captured body
    // with its original address, while no replacement or continuation fires.
    for (const bool replace_helpers : {false, true}) {
    for (unsigned failure = 0; failure < 13; ++failure) {
        for (unsigned behavior = 0; behavior < 3; ++behavior) {
            TestMathLibrary memory;
            std::map<uint32_t, Math::Body> table;
            std::vector<uint32_t> addresses, attempts;
            unsigned original_calls = 0, publication = 0;
            bool rejected = false;
            {
                Math math(memory);
                const Math::Capture capture = [&](uint32_t address) -> Math::Body {
                    addresses.push_back(address);
                    const Math::Body original = [&, address](CpuContext& context, uint32_t entry) {
                        ++original_calls;
                        context.cpuRegs.GPR.r[2].UL[0] = address ^ entry;
                    };
                    table[address] = original;
                    return original;
                };
                const Math::Installer install = [&](uint32_t address, Math::Body body) {
                    attempts.push_back(address);
                    if (rejected && behavior != 0) {
                        if (behavior == 2) throw std::runtime_error("restore rejected");
                        return false;
                    }
                    table[address] = std::move(body);
                    if (!rejected && publication++ == failure) {
                        rejected = true;
                        if (behavior == 2) throw std::runtime_error("install rejected");
                        return false;
                    }
                    return true;
                };
                bool threw = false;
                try {
                    CHECK(!math.Install(capture, install, replace_helpers));
                } catch (const std::runtime_error&) {
                    threw = true;
                }
                CHECK(threw == (behavior == 2));
                CHECK(attempts.size() == 2 * (failure + 1));
                for (unsigned index = 0; index <= failure; ++index) {
                    CHECK(attempts[index] == addresses[index]);
                    CHECK(attempts[failure + 1 + index] == addresses[failure - index]);
                }
                CHECK(math.Counts().transform_point == 0);
                CHECK(math.Counts().generated_continuations == 0);
            }
            // The inactive path must remain safe after MathLibrary destruction.
            // Invalid math pointers make accidental activation observable.
            for (const auto& [address, body] : table) {
                for (uint32_t entry : {0u, address, address + 4}) {
                    CpuContext context;
                    std::memset(&context, 0xa5, sizeof(context));
                    body(context, entry);
                    CHECK(context.cpuRegs.GPR.r[2].UL[0] == (address ^ entry));
                }
            }
            CHECK(original_calls == 39);
            CHECK(memory.reads.empty());
            CHECK(memory.writes.empty());
        }
    }
    }
    TestMathLibrary memory;
    Math math(memory);
    std::map<uint32_t, Math::Body> table;
    unsigned originals = 0;
    CHECK(math.Install([&](uint32_t) -> Math::Body {
        return [&](CpuContext&, uint32_t) { ++originals; };
    }, [&](uint32_t address, Math::Body body) {
        table[address] = std::move(body);
        CpuContext context{};
        table.at(Math::kTransformPointAddress)(context, 0);
        CHECK(math.CallCount() == 0);  // No activation while publication is partial.
        return true;
    }));
    CHECK(originals == 13);
    CHECK(math.Counts().generated_body_calls[0] == 13);
    CpuContext context{};
    table.at(Math::kTransformPointAddress)(context, 0);
    CHECK(math.CallCount() == 1);
    CHECK(originals == 13);
    CHECK(math.Counts().generated_body_calls[0] == 13);
}

void TestControlInstall() {
    TestMathLibrary memory;
    memory.Seed(matrix, {2, -3, 4, 1});
    Math math(memory);
    std::map<uint32_t, Math::Body> table;
    std::vector<uint32_t> addresses;
    std::array<unsigned, 13> entered{};
    CHECK(math.Install([&](uint32_t address) -> Math::Body {
        const size_t index = addresses.size();
        addresses.push_back(address);
        return [&, index, address](CpuContext& context, uint32_t entry) {
            CHECK(math.Counts().generated_body_calls[index] == ++entered[index]);
            context.cpuRegs.GPR.r[2].UL[0] = address ^ entry;
        };
    }, [&](uint32_t address, Math::Body body) {
        table[address] = std::move(body);
        return true;
    }, false));
    CHECK(addresses.size() == 13);
    for (size_t index = 0; index < addresses.size(); ++index) {
        const auto address = addresses[index];
        for (uint32_t entry : {0u, address, address + 4}) {
            CpuContext context{};
            context.vuRegs.VF[0].w = 1;
            context.cpuRegs.GPR.r[4].UL[0] = 0x100;
            context.cpuRegs.GPR.r[5].UL[0] = 0x200;
            context.cpuRegs.GPR.r[6].UL[0] = 0x300;
            table.at(address)(context, entry);
            if (index != 0 || entry == address + 4)
                CHECK(context.cpuRegs.GPR.r[2].UL[0] == (address ^ entry));
        }
        CHECK(math.Counts().generated_body_calls[index] == (index == 0 ? 1 : 3));
    }
    CHECK(math.Counts().transform_point == 2);
    CHECK(math.Counts().rotate_vector == 0 && math.Counts().triangle_plane == 0);
    CHECK(math.Counts().generated_continuations == 13);
    CHECK(math.Census().find("generated_body_rotate_vector=3") != std::string::npos);
    for (const bool selected : {false, true}) {
        Math throwing(memory);
        CHECK(throwing.Install([](uint32_t) -> Math::Body {
            return [](CpuContext&, uint32_t) { throw std::runtime_error("original body entered"); };
        }, [&](uint32_t address, Math::Body body) {
            table[address] = std::move(body);
            return true;
        }, selected));
        bool caught = false;
        CpuContext context{};
        try { table.at(Math::kRotateVectorAddress)(context, selected ? Math::kRotateVectorAddress + 4 : 0); }
        catch (const std::runtime_error&) { caught = true; }
        CHECK(caught && throwing.Counts().generated_body_calls[1] == 1);
        CHECK(throwing.Counts().generated_continuations == (selected ? 1 : 0));
        CHECK(throwing.Counts().rotate_vector == 0);
    }
}

void TestMatrixValues() {
    const Matrix identity = Math::EvaluateIdentity();
    const Matrix expected_identity{{{1, 0, 0, 0}, {0, 1, 0, 0},
                                    {0, 0, 1, 0}, {0, 0, 0, 1}}};
    CHECK(identity == expected_identity);
    // Hamilton product with noncommuting integer inputs; independently
    // expanded values also pin the final partial scalar dot product.
    const auto rotations = Math::EvaluateRotationProduct({1, 2, 3, 4}, {5, 6, 7, 8});
    ExpectVector(rotations.output, {24, 48, 48, -6});
    ExpectVector(rotations.products, {5, 12, 21, 32});
    ExpectVector(rotations.accumulator, {42, 55, 58, -1});
    ExpectVector(Math::EvaluateRotationProduct({5, 6, 7, 8}, {1, 2, 3, 4}).output,
                 {32, 32, 56, -6});
    const auto half_turn = Math::EvaluateRotationMatrix({0, 0, 1, 0});
    ExpectVector(half_turn.rows[0], {-1, 0, 0, 0});
    ExpectVector(half_turn.rows[1], {0, -1, 0, 0});
    ExpectVector(half_turn.rows[2], {0, 0, 1, 0});
    ExpectVector(half_turn.sum, {0, 0, 0, 0});
    ExpectVector(half_turn.difference, {0, 0, 0, 0});
    CHECK(half_turn.accumulator == (std::array<float, 3>{1, 1, 1}));
    Matrix raw = matrix;
    raw[1][2] = Float(0x7fa12345);
    raw[3][0] = Float(0xffc12345);
    raw[0][1] = Float(0x80000000);
    raw[2][3] = Float(0x0000807f);  // OLD R109b: a moved word must retain denormal payloads.
    const auto transposed = Math::EvaluateTranspose(raw);
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            CHECK(Bits(transposed[row][column]) == Bits(raw[column][row]));
    const auto restored = Math::EvaluateTranspose(transposed);
    CHECK(std::memcmp(restored.data(), raw.data(), sizeof(raw)) == 0);
    const Matrix rigid{{{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {2, 3, 4, 1}}};
    const auto inverse = Math::EvaluateRigidInverse(rigid);
    const Matrix inverse_expected{{{0, -1, 0, 0}, {1, 0, 0, 0},
                                   {0, 0, 1, 0}, {-3, 2, -4, 1}}};
    for (size_t row = 0; row < 4; ++row) ExpectVector(inverse.output[row], inverse_expected[row]);
    CHECK(inverse.accumulator == (std::array<float, 3>{-3, 2, 0}));
    CHECK(ExactIntegerProduct(rigid, inverse.output) == identity);
    CHECK(ExactIntegerProduct(inverse.output, rigid) == identity);
}

// Independent word-permutation expectation, including the live GPR scratch
// values. Inverse leaves r11 holding the untransposed translation input.
void ExpectTransposeRegisters(CpuContext& expected, const Matrix& input,
                              const Matrix& output, bool inverse) {
    for (size_t row = 0; row < 4; ++row) {
        const auto& value = row == 3 && inverse ? input[3] : output[row];
        std::memcpy(expected.cpuRegs.GPR.r[8 + row].UL, value.data(), 16);
    }
    const Vector temporaries[] = {
        {input[0][0], input[1][0], input[0][1], input[1][1]},
        {input[0][2], input[1][2], input[0][3], input[1][3]},
        {input[2][0], input[3][0], input[2][1], input[3][1]},
        {input[2][2], input[3][2], input[2][3], input[3][3]},
    };
    for (size_t i = 0; i < 4; ++i)
        std::memcpy(expected.cpuRegs.GPR.r[12 + i].UL, temporaries[i].data(), 16);
}

void TestExtendedAdapter(unsigned operation, uint32_t destination) {
    TestMathLibrary memory;
    const Matrix input{{{0, 1, 0, 7}, {-1, 0, 0, 8}, {0, 0, 1, 9}, {2, 3, 4, 1}}};
    memory.Seed(input, {0, 0, 1, 0});
    TestMathLibrary::PutVector(memory.bytes, 0x240, {1, 2, 3, 4});
    TestMathLibrary::PutVector(memory.bytes, 0x260, {5, 6, 7, 8});
    Math math(memory);
    CpuContext context;
    std::memset(&context, 0xa5, sizeof(context));
    SetRegister(context.vuRegs.VF[0], {0, 0, 0, 1});
    context.cpuRegs.GPR.r[4].UL[0] = destination | 3;
    context.cpuRegs.GPR.r[5].UL[0] = 0x10f;
    context.cpuRegs.GPR.r[6].UL[0] = 0x267;
    if (operation == 1) context.cpuRegs.GPR.r[5].UL[0] = 0x24f;
    if (operation == 2) context.cpuRegs.GPR.r[5].UL[0] = 0x20f;
    if (operation >= 3 && operation <= 5) {
        context.cpuRegs.GPR.r[4].UL[0] = 0x103;
        context.cpuRegs.GPR.r[5].UL[0] = destination | 15;
    }
    if (operation == 4 || operation == 7) destination = 0x100;
    if (operation == 7) context.cpuRegs.GPR.r[4].UL[0] = 0x103;
    CpuContext expected;
    std::memcpy(&expected, &context, sizeof(expected));
    auto bytes = memory.bytes;
    const auto store = [&](uint32_t offset, const Vector& value) {
        TestMathLibrary::PutVector(bytes, destination + offset, value);
    };
    std::vector<uint32_t> reads, writes;
    std::vector<uint32_t> rows;
    if (operation == 0) {
        const Matrix identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
        for (size_t row = 0; row < 3; ++row) SetRegister(expected.vuRegs.VF[28 + row], identity[row]);
        for (uint32_t row = 0; row < 4; ++row) store(row * 16, identity[row]);
        rows = {3, 2, 1, 0};
        math.IdentityMatrix(context);
    } else if (operation == 1) {
        SetRegister(expected.vuRegs.VF[31], {1, 2, 3, 4});
        SetRegister(expected.vuRegs.VF[30], {5, 6, 7, 8});
        SetRegister(expected.vuRegs.VF[1], {5, 12, 21, 32});
        SetRegister(expected.vuRegs.ACC, {42, 55, 58, -1});
        SetRegister(expected.vuRegs.VF[29], {24, 48, 48, -6});
        store(0, {24, 48, 48, -6});
        AppendVectorAddresses(reads, 0x240);
        AppendVectorAddresses(reads, 0x260);
        rows = {0};
        math.MultiplyRotations(context);
    } else if (operation == 2) {
        expected.cpuRegs.GPR.r[2].UD[0] = 0x3fb504f3;
        expected.vuRegs.I = 0x3fb504f3;
        SetRegister(expected.vuRegs.VF[1], {0, 0, 0, 0});
        SetRegister(expected.vuRegs.VF[2], {0, 0, 0, 0});
        expected.vuRegs.VF[3].y = 0;
        expected.vuRegs.ACC.x = expected.vuRegs.ACC.y = expected.vuRegs.ACC.z = 1;
        const Vector rotation[] = {{-1, 0, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}};
        for (size_t row = 0; row < 3; ++row) {
            SetRegister(expected.vuRegs.VF[28 + row], rotation[row]);
            store(static_cast<uint32_t>(row) * 16, rotation[row]);
        }
        AppendVectorAddresses(reads, 0x200);
        rows = {0, 2, 1};
        math.RotationMatrix(context);
    } else {
        Matrix source = input;
        if (operation == 5) source[3] = {0, 0, 0, 1};
        Matrix transpose{{{0, -1, 0, source[3][0]}, {1, 0, 0, source[3][1]},
                          {0, 0, 1, source[3][2]}, {7, 8, 9, 1}}};
        const bool inverse = operation >= 6;
        ExpectTransposeRegisters(expected, source, transpose, inverse);
        Matrix output_rows = transpose;
        if (inverse) {
            output_rows = {{{0, -1, 0, 0}, {1, 0, 0, 0},
                            {0, 0, 1, 0}, {-3, 2, -4, 1}}};
            for (size_t row = 0; row < 4; ++row)
                SetRegister(expected.vuRegs.VF[28 + row], output_rows[row]);
            expected.vuRegs.ACC.x = -3;
            expected.vuRegs.ACC.y = 2;
            expected.vuRegs.ACC.z = Float(0x80000000);
        } else if (operation == 5) output_rows[3] = {0, 0, 0, 1};
        for (uint32_t row = 0; row < 4; ++row) store(row * 16, output_rows[row]);
        for (uint32_t row = 0; row < (operation == 5 ? 3u : 4u); ++row)
            AppendVectorAddresses(reads, 0x100 + row * 16);
        rows = inverse ? std::vector<uint32_t>{3, 0, 1, 2} : std::vector<uint32_t>{0, 1, 2, 3};
        switch (operation) {
            case 3: math.Transpose(context); break;
            case 4: math.TransposeInPlace(context); break;
            case 5: math.TransposeRotation(context); break;
            case 6: math.InvertRigid(context); break;
            case 7: math.InvertRigidInPlace(context); break;
        }
    }
    for (uint32_t row : rows) AppendVectorAddresses(writes, destination + row * 16);
    CHECK(memory.bytes == bytes);
    CHECK(std::memcmp(&context, &expected, sizeof(context)) == 0);
    CHECK(memory.reads == reads);
    CHECK(memory.writes == writes);
    CHECK(!memory.read_after_write);
    const auto counts = math.Counts();
    const uint64_t new_counts[] = {counts.identity_matrix, counts.multiply_rotations, counts.rotation_matrix,
        counts.transpose, counts.transpose_in_place, counts.transpose_rotation,
        counts.invert_rigid, counts.invert_rigid_in_place};
    for (size_t i = 0; i < 8; ++i) CHECK(new_counts[i] == (i == operation ? 1u : 0u));
}

void TestTrianglePlane(uint32_t destination, bool degenerate) {
    TestMathLibrary memory;
    memory.bytes.fill(0xa5);
    const Vector first{0, 0, 2, 1};
    const Vector second{1, 0, 2, 1};
    const Vector third = degenerate ? Vector{2, 0, 2, 1} : Vector{0, 1, 2, 1};
    const Vector plane = degenerate ? Vector{0, 0, 0, 0} : Vector{0, 0, 1, -2};
    TestMathLibrary::PutVector(memory.bytes, 0x100, first);
    TestMathLibrary::PutVector(memory.bytes, 0x120, second);
    TestMathLibrary::PutVector(memory.bytes, 0x140, third);
    const auto pure = Math::EvaluateTrianglePlane(first, second, third);
    ExpectVector(pure.plane, plane);
    CHECK(pure.valid == !degenerate);
    CHECK(Bits(pure.reciprocal_length) == (degenerate ? 0x7f7fffffu : 0x3f800000u));
    CpuContext context;
    std::memset(&context, 0xa5, sizeof(context));
    context.cpuRegs.GPR.r[0].UD[0] = 0;
    context.cpuRegs.GPR.r[4].UL[0] = destination | 3;
    context.cpuRegs.GPR.r[5].UL[0] = 0x107;
    context.cpuRegs.GPR.r[6].UL[0] = 0x127;
    context.cpuRegs.GPR.r[7].UL[0] = 0x147;
    context.cpuRegs.GPR.r[29].UD[0] = 0x510;
    SetRegister(context.vuRegs.VF[0], {0, 0, 0, 1});
    context.vuRegs.VF[11].I[3] = 0x7fa98765;
    CpuContext expected;
    std::memcpy(&expected, &context, sizeof(expected));
    SetRegister(expected.vuRegs.VF[8], first);
    SetRegister(expected.vuRegs.VF[9], degenerate ? Vector{0, 0, 0, 0} : Vector{2, 0, 2, 0});
    SetRegister(expected.vuRegs.VF[10], plane);
    expected.vuRegs.VF[11].x = degenerate ? 0 : 1;
    expected.vuRegs.VF[11].y = 0;
    expected.vuRegs.VF[11].z = degenerate ? 0 : 1;
    expected.vuRegs.ACC.x = expected.vuRegs.ACC.y = 0;
    expected.vuRegs.ACC.z = degenerate ? 0 : 1;
    expected.vuRegs.Q = degenerate ? Float(0x7f7fffff) : 1;
    expected.cpuRegs.GPR.r[1].UD[0] = 0x3851b717;
    expected.fpuRegs.fpr[1].UL = 0x3851b717;
    const bool success = !degenerate && destination != 0x500;
    expected.fpuRegs.fpr[0].f = success ? 1 : 0;
    expected.fpuRegs.fprc[31] = (expected.fpuRegs.fprc[31] & ~0x800000u) | (success ? 0x800000u : 0u);
    expected.cpuRegs.GPR.r[2].UD[0] = success ? 1 : 0;
    auto bytes = memory.bytes;
    TestMathLibrary::PutVector(bytes, 0x500,
        {degenerate ? 0.0f : 1.0f, 0, degenerate ? 0.0f : 1.0f, Float(0x7fa98765)});
    TestMathLibrary::PutVector(bytes, destination, plane);
    Math math(memory);
    math.TrianglePlane(context);
    CHECK(memory.bytes == bytes);
    CHECK(std::memcmp(&context, &expected, sizeof(context)) == 0);
    CHECK(math.Counts().triangle_plane == 1);
    CHECK(memory.read_after_write);  // The length is re-read after the plane store.
    std::vector<uint32_t> reads, writes;
    for (uint32_t address : {0x100u, 0x120u, 0x140u}) AppendVectorAddresses(reads, address);
    reads.push_back(0x500);
    AppendVectorAddresses(writes, 0x500);
    AppendVectorAddresses(writes, destination);
    CHECK(memory.reads == reads);
    CHECK(memory.writes == writes);
}

void TestTriangleRounding() {
    // Saturated squared length. sqrt(MAX_FLOAT) straddles the last float below
    // 2^64: unlike the EE scalar sqrt helper, this macro operation retains the
    // caller's rounding mode. This pins the overflow path caught by the oracle.
    const Vector origin{0, 0, 0, 1};
    const Vector x{Float(0x71800000), 0, 0, 1};  // 2^100
    const Vector y{0, 1, 0, 1};
    const int saved = std::fegetround();
    for (int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD}) {
        CHECK(std::fesetround(mode) == 0);
        const auto result = Math::EvaluateTrianglePlane(origin, x, y);
        CHECK(Bits(result.length_components[0]) == (mode == FE_UPWARD ? 0x5f800000u : 0x5f7fffffu));
        CHECK(Bits(result.reciprocal_length) == (mode == FE_TONEAREST ? 0x1f800001u : 0x1f800000u));
        CHECK(result.valid);
        CHECK(std::fegetround() == mode);
    }
    CHECK(std::fesetround(saved) == 0);
}

}  // namespace

int main() {
    const int original_rounding = std::fegetround();
    CHECK(std::fesetround(FE_TONEAREST) == 0);
    TestValueArguments();
    TestNumericEdges();
    TestSeparateRounding();
    TestAdapter(0x300, false, 1);
    TestAdapter(0x200, false, 1);  // TransformCollisionHit transforms in place.
    for (uint32_t row = 0; row < 4; ++row)
        TestAdapter(0x100 + row * 16, false, 1);  // Output aliases each matrix row.
    TestAdapter(0x200, true, 1);
    TestAdapter(0x300, false, 2);  // Preserve the context's translation factor.
    TestRawInputsAndCounter();
    TestAdditionalValueArguments();
    TestAdditionalNumericEdges();
    for (uint32_t destination : {0x300u, 0x200u, 0x100u, 0x110u, 0x120u, 0x130u}) {
        TestVectorAdapter(destination, true);
        TestVectorAdapter(destination, false);
    }
    for (uint32_t destination : {0x300u, 0x100u, 0x110u, 0x200u, 0x210u}) {
        TestMatrixAdapter(destination, false);
        TestMatrixAdapter(destination, true);
    }
    TestInstallFailure();
    TestDefaultInstall();
    TestCompleteInstall();
    TestControlInstall();
    TestInstallRollback();
    TestMatrixValues();
    for (unsigned operation = 0; operation < 8; ++operation)
        for (uint32_t destination : {0x300u, 0x100u, 0x110u, 0x200u, 0x240u, 0x260u})
            TestExtendedAdapter(operation, destination);
    for (bool degenerate : {false, true})
        for (uint32_t destination : {0x300u, 0x100u, 0x120u, 0x140u, 0x500u})
            TestTrianglePlane(destination, degenerate);
    TestTriangleRounding();
    CHECK(std::fesetround(original_rounding) == 0);
    std::printf("math_library: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
