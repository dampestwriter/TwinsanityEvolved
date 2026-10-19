#include "games/slus_20909_crash_twinsanity/hle/math/culling_state_codec.h"
#include "engine/vu/tiers/vu_interpreter_tier.h"
#include "engine/vu/vu_unit.h"
#include "math_library_culling_image.h"

#include <cfenv>
#include <cstdio>
#include <cstring>

namespace {
using Math = title::CullingMath;
using State = title::CullingState;
using Codec = title::CullingStateCodec;
unsigned checks = 0, failures = 0;
uint64_t original_calls = 0;

void Check(bool value, const char* text) {
    ++checks;
    if (!value && failures++ < 12) std::fprintf(stderr, "%s\n", text);
}
Math::Matrix Identity() {
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}
uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
float Input(uint32_t& seed, bool raw) {
    const auto word = Next(seed);
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return raw ? value : static_cast<float>(static_cast<int32_t>(word) / 65536) / 256.0f;
}
void Run(VuUnit& control, uint32_t entry) {
    VuInterpreterTier original;
    ++original_calls;
    original.Run(control, entry);
}
void Link(VuUnit& control, const Math::View& view, const Math::Matrix& chunk, const Math::Matrix& object) {
    control.WriteDataMem(0x980, reinterpret_cast<const uint8_t*>(&view), sizeof(view));
    std::memcpy(&control.regs().VF[3], &chunk, 64);
    std::memcpy(&control.regs().VF[7], &object, 64);
    Run(control, 0x398);
}
Math::Frustum Planes() {
    Math::Frustum planes{};
    planes.sides.normals[0] = {1, -1, 0, 0};
    planes.sides.normals[1] = {0, 0, 1, -1};
    planes.sides.absolute_normals[0] = {1, 1, 0, 0};
    planes.sides.absolute_normals[1] = {0, 0, 1, 1};
    planes.sides.offsets = {10, 10, 10, 10};
    planes.far_sides = planes.sides;
    planes.far_sides.offsets = {50, 50, 50, 50};
    planes.depth.normals[2] = {1, -1, 0, 0};
    planes.depth.absolute_normals[2] = {1, 1, 0, 0};
    planes.depth.offsets = {10, 10, 0, 0};
    return planes;
}
void Planes(VuUnit& control, const Math::Frustum& planes) {
    control.WriteDataMem(0x800, reinterpret_cast<const uint8_t*>(&planes.sides), sizeof(planes.sides));
    control.WriteDataMem(0x870, reinterpret_cast<const uint8_t*>(&planes.far_sides), sizeof(planes.far_sides));
    control.WriteDataMem(0x8e0, reinterpret_cast<const uint8_t*>(&planes.depth), sizeof(planes.depth));
}
std::vector<uint8_t> Bytes(const State& state) {
    std::vector<uint8_t> bytes;
    Check(Codec::Encode(state.Capture(), bytes), "owned culling snapshot encodes");
    return bytes;
}
void RoundTrip(State& state) {
    const auto bytes = Bytes(state);
    State::Snapshot decoded;
    Check(Codec::Decode(bytes, decoded), "owned culling snapshot decodes");
    State restored;
    auto other = Identity();
    other[3] = {43, 37, 29, 1};
    restored.LoadView({other, other, {31, 23, 19, 1}});
    Check(restored.Restore(decoded), "restore replaces unrelated boot state");
    Check(Bytes(restored) == bytes, "all raw owned state survives exact round trip");
    state = restored;
}
int32_t Distance(const VuUnit& unit) {
    int32_t value;
    std::memcpy(&value, &unit.regs().VF[15].I[0], 4);
    return value;
}
void CheckBox(const State& selected, const VuUnit& control) {
    const auto mesh = selected.MeshResult();
    const auto instance = selected.InstanceResult();
    Check(mesh.has_value() && instance.has_value(), "box result has both source readers");
    if (!mesh || !instance) return;
    const auto outside = control.regs().VI[1], clipped = control.regs().VI[2];
    Check(mesh->visibility.outside == outside && mesh->visibility.clipped == clipped &&
        mesh->distance == Distance(control), "owned box outcome matches actual control");
    Check(instance->visibility.outside == outside && instance->visibility.clipped == clipped &&
        instance->distance == Distance(control) && !instance->to_screen && !instance->to_clip,
        "instance reader takes distance and visibility without matrices");
    Check(mesh->to_screen.has_value() == (outside == 0) &&
        mesh->to_clip.has_value() == (outside == 0 && clipped != 0), "matrix validity matches source branch");
    if (mesh->to_screen) Check(std::memcmp(&*mesh->to_screen, &control.regs().VF[11], 64) == 0,
        "owned screen rows match control");
    if (mesh->to_clip) Check(std::memcmp(&*mesh->to_clip, &control.regs().VF[7], 64) == 0,
        "owned clip rows match control");
}
void SnapshotPins() {
    State state;
    const auto empty = Bytes(state);
    Check(empty == std::vector<uint8_t>({'C', 'U', 'L', 'L', 1, 0, 0, 0, 0, 0, 0, 0}),
        "empty snapshot has canonical little-endian header and explicit empty fields");
    const Math::Box box{{-1, -1, -1, 1}, {1, 1, 1, 1}};
    Check(!state.LoadLink(Identity(), Identity()) && !state.LoadModel(Identity()) &&
        !state.TestCell(box) && !state.TestBoxAt(box, Identity()) && Bytes(state) == empty,
        "missing producers fail atomically, not with invented default results");
    state.LoadPlanes(Planes());
    Check(state.TestCell(box), "cell classification owns planes independently of view");
    RoundTrip(state);
    Check(state.CellResult().has_value() && !state.MeshResult() && !state.DrawnModelResult(),
        "cell results cannot supply another reader's stale registers");
    state.LoadView({Identity(), Identity(), {3, 4, 12, 1}});
    Check(!state.CellResult() && !state.MeshResult(), "a new view invalidates the previous result");
    Check(state.TestBoxAt(box, Identity()), "complete view and plane state allow box tests");
    const auto valid = state.Capture();
    const auto kept = Bytes(state);
    auto invalid = valid;
    invalid.version = 99;
    Check(!state.Restore(invalid) && Bytes(state) == kept, "unknown state version leaves live state unchanged");
    invalid = valid; invalid.view.reset();
    Check(!state.Restore(invalid), "a link without its source view cannot be restored");
    invalid = valid; invalid.planes.reset();
    Check(!state.Restore(invalid), "a box result without its planes cannot be restored");
    invalid = valid; std::get<Math::BoxView>(invalid.result).to_screen.reset();
    Check(!state.Restore(invalid), "visible box requires its screen output");
    invalid = valid; std::get<Math::BoxView>(invalid.result).to_clip = Identity();
    Check(!state.Restore(invalid), "unclipped box cannot publish stale clip output");
    invalid = valid; std::get<Math::BoxView>(invalid.result).visibility.outside = 1;
    Check(!state.Restore(invalid), "reserved plane mask bits cannot be restored");
    invalid = valid; std::get<Math::BoxView>(invalid.result).distance = -1;
    Check(!state.Restore(invalid), "negative squared distance is malformed state");
    auto encode_output = kept;
    Check(!Codec::Encode(invalid, encode_output) && encode_output == kept, "encode failure preserves output");
    auto output = valid;
    for (size_t size = 0; size < kept.size(); ++size) {
        const std::vector<uint8_t> truncated(kept.begin(), kept.begin() + size);
        Check(!Codec::Decode(truncated, output), "every truncated owned snapshot is rejected");
    }
    auto trailing = kept; trailing.push_back(0);
    Check(!Codec::Decode(trailing, output), "trailing bytes are not ignored");
    auto noncanonical = kept; noncanonical[8] = 2;
    Check(!Codec::Decode(noncanonical, output), "boolean encoding must be canonical");
    auto unknown = empty; unknown.back() = 4;
    Check(!Codec::Decode(unknown, output), "unknown result tag is rejected");
    std::vector<uint8_t> after;
    Check(Codec::Encode(output, after) && after == kept, "all decode failures leave output intact");
    state.Reset();
    RoundTrip(state);
    Check(Bytes(state) == empty, "reset and restore remove every old view, plane and result");
}
void Sequence(int mode) {
    Check(std::fesetround(mode) == 0, "set rounding");
    uint32_t seed = 0x53544154;
    for (unsigned trial = 0; trial < 600; ++trial) {
        Math::View view{Identity(), Identity(), {3, 4, 12, 1}};
        Math::Matrix chunk = Identity(), object = Identity(), model = Identity();
        if (trial % 5) {
            for (auto* matrix : {&view.to_clip, &view.to_screen, &chunk, &object, &model})
                for (auto& row : *matrix) for (auto& lane : row) lane = Input(seed, trial % 3 == 0);
            for (auto& lane : view.camera) lane = Input(seed, trial % 3 == 0);
        }
        const auto planes = Planes();
        const Math::Box box{{-1, -1, -1, 1}, {1, 1, 1, 1}};
        VuUnit control(0);
        control.WriteMicroMem(0, math_culling_oracle::kImage.data(), math_culling_oracle::kImage.size());
        State selected;
        Link(control, view, Identity(), Identity());
        selected.LoadView(view);
        RoundTrip(selected);
        Planes(control, planes); selected.LoadPlanes(planes);
        Link(control, view, chunk, object);
        Check(selected.LoadLink(chunk, object), "linked source view updates");
        RoundTrip(selected);
        std::memcpy(&control.regs().VF[1], &box, sizeof(box));
        Run(control, 0x1c0);
        const auto before = original_calls;
        Check(selected.TestCell(box), "cell producer runs");
        RoundTrip(selected);
        const auto cell = selected.CellResult();
        Check(cell && cell->outside == control.regs().VI[1] && cell->clipped == control.regs().VI[2],
            "cell outcome survives restore and matches actual source control");
        Check(original_calls == before, "native cell and restore execute no original model");
        std::memcpy(&control.regs().VF[1], &box, sizeof(box));
        std::memcpy(&control.regs().VF[3], &model, 64);
        Run(control, 0x500);
        Check(selected.TestBoxAt(box, model), "box producer runs");
        RoundTrip(selected);
        CheckBox(selected, control);
        Check(!selected.CellResult() && !selected.DrawnModelResult(), "wrong reader cannot take pending box state");
        std::memcpy(&control.regs().VF[3], &model, 64);
        Run(control, 0x2d0);
        Check(selected.LoadModel(model), "drawn model producer runs");
        RoundTrip(selected);
        const auto drawn = selected.DrawnModelResult();
        Check(drawn && drawn->distance == Distance(control) &&
            std::memcmp(&drawn->to_screen, &control.regs().VF[11], 64) == 0,
            "drawn model distance and screen rows survive restore and match actual control");
        const auto saved = drawn;
        Check(selected.TestBoxAt(box, Identity()), "next item starts after previous result is read");
        Check(drawn && saved && std::memcmp(&drawn->to_screen, &saved->to_screen, 64) == 0,
            "queued next-item work cannot mutate an owned previous result");
        Check(std::fegetround() == mode, "state and codec restore caller rounding");
    }
}
}

int main() {
    SnapshotPins();
    const auto mode = std::fegetround();
    for (int rounding : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) Sequence(rounding);
    std::fesetround(mode);
    std::printf("MathLibraryCullingState checks=%u failures=%u actual_control_calls=%llu sequences=2400\n",
        checks, failures, static_cast<unsigned long long>(original_calls));
    return failures ? 1 : 0;
}
