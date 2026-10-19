#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "math_library_culling_image.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
using Math = title::CullingMath;
unsigned checks = 0, failures = 0;
uint64_t original_link_calls = 0, original_model_calls = 0;
uint64_t selected_link_calls = 0, selected_model_calls = 0;
uint64_t original_box_calls = 0, selected_box_calls = 0;
uint64_t original_box_at_calls = 0, selected_box_at_calls = 0;
uint64_t control_rejected = 0, control_screen_only = 0, control_clip_screen = 0;

void Check(bool good, const char* text) {
    ++checks;
    if (!good && failures++ < 12) std::fprintf(stderr, "%s\n", text);
}

uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

Math::Matrix Identity() {
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

void MatrixInput(Math::Matrix& matrix, uint32_t& seed, unsigned trial) {
    for (auto& row : matrix) {
        for (auto& lane : row) {
            const uint32_t bits = Next(seed);
            lane = trial % 3 ? static_cast<float>(static_cast<int32_t>(bits) / 65536) / 256.0f : Float(bits);
        }
    }
    if (trial % 7 == 0) {
        matrix[0][3] = matrix[1][3] = matrix[2][3] = 0;
        matrix[3][3] = 1;
    }
    if (trial % 23 == 0) matrix = Identity();
}

Math::Link ControlLink(VuUnit& unit, const Math::View& view,
        const Math::Matrix& chunk, const Math::Matrix& object) {
    unit.WriteDataMem(0x980, reinterpret_cast<const uint8_t*>(&view.to_clip), 64);
    unit.WriteDataMem(0x9c0, reinterpret_cast<const uint8_t*>(&view.to_screen), 64);
    unit.WriteDataMem(0xa00, reinterpret_cast<const uint8_t*>(&view.camera), 16);
    std::memcpy(&unit.regs().VF[3], &chunk, 64);
    std::memcpy(&unit.regs().VF[7], &object, 64);
    VuInterpreterTier original;
    ++original_link_calls;  // Immediately at the actual NEW operation execution.
    original.Run(unit, 0x398);
    Math::Link link;
    unit.ReadDataMem(0, reinterpret_cast<uint8_t*>(&link.to_clip), 64);
    unit.ReadDataMem(64, reinterpret_cast<uint8_t*>(&link.to_screen), 64);
    unit.ReadDataMem(128, reinterpret_cast<uint8_t*>(&link.camera), 16);
    Check(unit.regs().TPC == 0x4f8, "actual link control reached its reviewed return");
    return link;
}

Math::Model ControlModel(VuUnit& unit, const Math::Matrix& matrix) {
    std::memcpy(&unit.regs().VF[3], &matrix, 64);
    VuInterpreterTier original;
    ++original_model_calls;
    original.Run(unit, 0x2d0);
    Math::Model model;
    std::memcpy(&model.to_screen, &unit.regs().VF[11], 64);
    std::memcpy(&model.distance, &unit.regs().VF[15].I[0], 4);
    Check(unit.regs().TPC == 0x398, "actual model control reached its reviewed return");
    return model;
}

Math::Classification ControlBox(VuUnit& unit, const Math::Frustum& frustum, const Math::Box& box) {
    unit.WriteDataMem(0x800, reinterpret_cast<const uint8_t*>(&frustum.sides), sizeof(frustum.sides));
    unit.WriteDataMem(0x8e0, reinterpret_cast<const uint8_t*>(&frustum.depth), sizeof(frustum.depth));
    std::memcpy(&unit.regs().VF[1], &box.minimum, 16);
    std::memcpy(&unit.regs().VF[2], &box.maximum, 16);
    VuInterpreterTier original;
    ++original_box_calls;
    original.Run(unit, 0x1c0);
    Check(unit.regs().TPC == 0x2d0, "actual box control reached its reviewed return");
    return {unit.regs().VI[1], unit.regs().VI[2]};
}

Math::BoxView ControlBoxAt(VuUnit& unit, const Math::Frustum& frustum,
        const Math::Box& box, const Math::Matrix& model) {
    unit.WriteDataMem(0x800, reinterpret_cast<const uint8_t*>(&frustum.sides), sizeof(frustum.sides));
    unit.WriteDataMem(0x870, reinterpret_cast<const uint8_t*>(&frustum.far_sides), sizeof(frustum.far_sides));
    unit.WriteDataMem(0x8e0, reinterpret_cast<const uint8_t*>(&frustum.depth), sizeof(frustum.depth));
    std::memcpy(&unit.regs().VF[1], &box.minimum, 16);
    std::memcpy(&unit.regs().VF[2], &box.maximum, 16);
    std::memcpy(&unit.regs().VF[3], &model, 64);
    VuInterpreterTier original;
    ++original_box_at_calls;
    original.Run(unit, 0x500);
    Math::BoxView result{};
    result.visibility = {unit.regs().VI[1], unit.regs().VI[2]};
    std::memcpy(&result.distance, &unit.regs().VF[15].I[0], 4);
    uint32_t end = 0x848;
    if (result.visibility.outside) ++control_rejected;
    if (!result.visibility.outside) {
        Math::Matrix screen;
        std::memcpy(&screen, &unit.regs().VF[11], 64);
        result.to_screen = screen;
        end = 0x8e8;
        if (result.visibility.clipped) {
            ++control_clip_screen;
            Math::Matrix clip;
            std::memcpy(&clip, &unit.regs().VF[7], 64);
            result.to_clip = clip;
            end = 0xa08;
        } else ++control_screen_only;
    }
    Check(unit.regs().TPC == end, "transformed box takes its source result-validity return");
    return result;
}

void BoxPins() {
    Math::Frustum frustum{};
    frustum.sides.normals[0] = {1, -1, 0, 0};
    frustum.sides.normals[1] = {0, 0, 1, -1};
    frustum.sides.absolute_normals[0] = {1, 1, 0, 0};
    frustum.sides.absolute_normals[1] = {0, 0, 1, 1};
    frustum.sides.offsets = {10, 10, 10, 10};
    frustum.depth.normals[2] = {1, -1, 0, 0};
    frustum.depth.absolute_normals[2] = {1, 1, 0, 0};
    frustum.depth.offsets = {10, 10, 0, 0};
    frustum.far_sides = frustum.sides;
    frustum.far_sides.offsets = {50, 50, 50, 50};
    const Math::Box inside{{-1, -1, -1, Float(0x7fc12345)}, {1, 1, 1, Float(0xff800000)}};
    auto result = Math::TestBox(frustum, inside);
    Check(result.outside == 0 && result.clipped == 0, "box wholly inside six planes; corner.w ignored");
    result = Math::TestBox(frustum, {{9, -1, -1, 0}, {11, 1, 1, 0}});
    Check(result.outside == 0 && result.clipped == 0x40, "box straddles right plane");
    result = Math::TestBox(frustum, {{11, -1, -1, 0}, {12, 1, 1, 0}});
    Check(result.outside == 0x40 && result.clipped == 0x40, "box wholly outside right plane");
    result = Math::TestBox(frustum, {{-1, -1, 11, 0}, {1, 1, 12, 0}});
    Check(result.outside == 0x40 && result.clipped == 0x40, "depth columns contribute the same source mask");
    Math::Frustum signed_zero{};
    for (auto& normal : signed_zero.sides.normals) normal.fill(Float(0x80000000));
    signed_zero.sides.offsets.fill(Float(0x80000000));
    result = Math::TestBox(signed_zero, {{1, 1, 1, 0}, {3, 3, 3, 0}});
    Check(result.outside == 0 && result.clipped == 0xf0,
        "negative-zero low support distances retain source sign classification");
    const Math::Link link{Identity(), Identity(), {3, 4, 12, 1}};
    auto placed = Math::TestBoxAt(frustum, link, inside, Identity());
    Check(!placed.visibility.outside && !placed.visibility.clipped &&
        placed.distance == 169 && placed.to_screen == Identity() && !placed.to_clip,
        "visible placed box produces only the screen matrix and origin-based LOD");
    placed = Math::TestBoxAt(frustum, link, {{-1, -1, -11, 0}, {1, 1, -9, 0}}, Identity());
    Check(!placed.visibility.outside && placed.visibility.clipped == 0x80 &&
        placed.to_screen == Identity() && placed.to_clip == Identity(),
        "near intersection keeps both source matrices");
    placed = Math::TestBoxAt(frustum, link, {{-1, -1, 9, 0}, {1, 1, 11, 0}}, Identity());
    Check(placed.visibility.outside == 0x40 && !placed.to_screen && !placed.to_clip,
        "far intersection is rejected before either matrix is published");
    placed = Math::TestBoxAt(frustum, link, {{9, -1, -1, 0}, {11, 1, 1, 0}}, Identity());
    Check(!placed.visibility.outside && !placed.visibility.clipped && placed.to_screen && !placed.to_clip,
        "source wider side planes decide matrix clipping after ordinary side rejection");
}

void BoxTrial(VuUnit& original, const Math::Link& link, const Math::Matrix& model,
        uint32_t& seed, unsigned trial) {
    Math::Frustum frustum;
    for (auto* set : {&frustum.sides, &frustum.depth, &frustum.far_sides}) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            for (unsigned plane = 0; plane < 4; ++plane) {
                const uint32_t bits = Next(seed);
                const float normal = trial % 3 ? static_cast<float>(static_cast<int32_t>(bits) / 65536) / 256.0f
                    : Float(bits);
                set->normals[axis][plane] = normal;
                set->absolute_normals[axis][plane] = trial % 5 ? std::fabs(normal) : Float(Next(seed));
            }
        }
        for (auto& offset : set->offsets)
            offset = trial % 3 ? static_cast<float>(static_cast<int32_t>(Next(seed)) / 65536) : Float(Next(seed));
    }
    Math::Box box;
    for (unsigned lane = 0; lane < 4; ++lane) {
        box.minimum[lane] = trial % 3 ? static_cast<float>(static_cast<int32_t>(Next(seed)) / 65536) : Float(Next(seed));
        box.maximum[lane] = trial % 3 ? box.minimum[lane] + static_cast<float>(Next(seed) % 10000) : Float(Next(seed));
    }
    if (trial % 32 == 0) {
        frustum = {};
        for (auto& normal : frustum.sides.normals) normal.fill(Float(0x80000000));
        frustum.sides.offsets.fill(Float(0x80000000));
        box = {{1, 1, 1, Float(0x807f)}, {3, 3, 3, Float(0xff800000)}};
    }
    const auto control = ControlBox(original, frustum, box);
    const auto before = original_box_calls;
    ++selected_box_calls;
    const auto selected = Math::TestBox(frustum, box);
    Check(control.outside == selected.outside && control.clipped == selected.clipped,
        "source box classification differs from actual NEW control");
    Check(before == original_box_calls, "selected box executes no original model");
    const auto control_at = ControlBoxAt(original, frustum, box, model);
    const auto original_before = original_box_at_calls;
    ++selected_box_at_calls;
    const auto selected_at = Math::TestBoxAt(frustum, link, box, model);
    Check(control_at.visibility.outside == selected_at.visibility.outside &&
        control_at.visibility.clipped == selected_at.visibility.clipped &&
        control_at.distance == selected_at.distance, "transformed box classification/distance differs from NEW");
    const auto equal = [](const auto& a, const auto& b) {
        return a.has_value() == b.has_value() && (!a || std::memcmp(&*a, &*b, sizeof(*a)) == 0);
    };
    Check(equal(control_at.to_screen, selected_at.to_screen) && equal(control_at.to_clip, selected_at.to_clip),
        "transformed box valid matrices differ from NEW");
    Check(original_before == original_box_at_calls, "selected transformed box executes no original model");
}

void Pins() {
    const Math::Matrix identity = Identity();
    Math::View view{identity, identity, {3, 4, 12, Float(0x7fc12345)}};
    const Math::Link link = Math::LoadLink(view, identity, identity);
    Check(link.to_clip == identity && link.to_screen == identity, "identity view matrices");
    Check(link.camera == Math::Vector{3, 4, 12, 1}, "camera.w ignored and source object translation applied");
    Check(Math::LoadModel(link, identity).distance == 169, "LOD is squared xyz distance, not a root");
    Math::Matrix translated = identity;
    translated[3] = {3, 4, 12, Float(0xff800000)};
    Check(Math::LoadModel(link, translated).distance == 0, "model position.w ignored for distance");
    view.camera = {65536, 0, 0, 1};
    Check(Math::LoadModel(Math::LoadLink(view, identity, identity), identity).distance ==
        std::numeric_limits<int32_t>::max(), "squared distance saturates before integer conversion");
    view.camera = {Float(0x807f), Float(0x8000807f), 0, Float(0x807f)};
    Check(Math::LoadModel(Math::LoadLink(view, identity, identity), identity).distance == 0,
        "denormals flush only when consumed as arithmetic");
    Math::Matrix chunk = identity, object = identity;
    chunk[3] = {2, 5, 7, 1};
    object[3] = {11, 13, 17, 1};
    view.camera = {1, 2, 3, 91};
    const auto linked = Math::LoadLink(view, chunk, object);
    Check(linked.to_screen[3] == chunk[3] && linked.camera == Math::Vector{12, 15, 20, 1},
        "chunk composes the matrices; object separately transforms the camera");
    view.to_screen = identity;
    view.to_screen[0][0] = -1;
    view.to_screen[1][0] = Float(0x3f800001);
    chunk = identity;
    chunk[0] = {1, Float(0x3f7ffffe), 0, 0};
    Check(Math::LoadLink(view, chunk, object).to_screen[0][0] == Float(0xa8800000),
        "ordered fused accumulation preserves -2^-46 cancellation, not an unfused product");
    auto fractional = link;
    fractional.camera = {1.75f, 0, 0, 1};
    Check(Math::LoadModel(fractional, identity).distance == 3,
        "squared LOD distance truncates a fraction rather than rounding or taking a root");
}

void Sweep(int mode) {
    Check(std::fesetround(mode) == 0, "set caller rounding");
    Pins();
    BoxPins();
    uint32_t seed = 0x43554c4c;
    for (unsigned trial = 0; trial < 4000; ++trial) {
        Math::View view;
        Math::Matrix chunk, object, model;
        MatrixInput(view.to_clip, seed, trial);
        MatrixInput(view.to_screen, seed, trial + 1);
        MatrixInput(chunk, seed, trial + 2);
        MatrixInput(object, seed, trial + 3);
        MatrixInput(model, seed, trial + 4);
        for (auto& lane : view.camera)
            lane = trial % 3 ? static_cast<float>(static_cast<int32_t>(Next(seed)) / 65536) : Float(Next(seed));
        const Math::View saved_view = view;
        const auto saved_chunk = chunk, saved_object = object, saved_model = model;
        VuUnit original(0);
        original.WriteMicroMem(0, math_culling_oracle::kImage.data(), math_culling_oracle::kImage.size());
        for (auto& row : original.regs().VF) for (auto& lane : row.I) lane = Next(seed);
        const auto control_link = ControlLink(original, view, chunk, object);
        const auto control_model = ControlModel(original, model);
        const uint64_t bodies_before = original_link_calls + original_model_calls;
        ++selected_link_calls;
        const auto selected_link = Math::LoadLink(view, chunk, object);
        ++selected_model_calls;
        const auto selected_model = Math::LoadModel(selected_link, model);
        Check(std::memcmp(&selected_link, &control_link, sizeof(selected_link)) == 0,
            "source link matrices/camera differ from actual NEW control");
        Check(std::memcmp(&selected_model.to_screen, &control_model.to_screen, 64) == 0 &&
            selected_model.distance == control_model.distance,
            "source model matrix/squared distance differ from actual NEW control");
        Check(original_link_calls + original_model_calls == bodies_before, "selected arm executes no original model");
        Check(std::memcmp(&view, &saved_view, sizeof(view)) == 0 &&
            std::memcmp(&chunk, &saved_chunk, sizeof(chunk)) == 0 &&
            std::memcmp(&object, &saved_object, sizeof(object)) == 0 &&
            std::memcmp(&model, &saved_model, sizeof(model)) == 0, "ordinary math inputs remain unchanged");
        BoxTrial(original, selected_link, model, seed, trial);
        Check(std::fegetround() == mode, "source operations restore caller rounding");
    }
}
}  // namespace

int main() {
    const int previous = std::fegetround();
    for (const int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) Sweep(mode);
    std::fesetround(previous);
    Check(control_rejected > 0, "actual source control exercises rejected transformed boxes");
    Check(control_screen_only > 0, "actual source control exercises screen-only transformed boxes");
    Check(control_clip_screen > 0, "actual source control exercises clip-plus-screen transformed boxes");
    std::printf("MathLibraryCullingOracle checks=%u failures=%u source_comparisons=64000\n", checks, failures);
    std::printf("actual_control_link=%llu actual_control_model=%llu selected_link=%llu selected_model=%llu\n",
        static_cast<unsigned long long>(original_link_calls), static_cast<unsigned long long>(original_model_calls),
        static_cast<unsigned long long>(selected_link_calls), static_cast<unsigned long long>(selected_model_calls));
    std::printf("actual_control_box=%llu selected_box=%llu\n",
        static_cast<unsigned long long>(original_box_calls), static_cast<unsigned long long>(selected_box_calls));
    std::printf("actual_control_box_at=%llu selected_box_at=%llu\n",
        static_cast<unsigned long long>(original_box_at_calls), static_cast<unsigned long long>(selected_box_at_calls));
    std::printf("control_rejected=%llu control_screen_only=%llu control_clip_screen=%llu\n",
        static_cast<unsigned long long>(control_rejected), static_cast<unsigned long long>(control_screen_only),
        static_cast<unsigned long long>(control_clip_screen));
    std::printf("ELF sha256=%s\nimage sha256=%s\n", math_culling_oracle::kElfDigest, math_culling_oracle::kImageDigest);
    return failures ? 1 : 0;
}
