#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_planes_oracle.h"
#include "games/slus_20909_crash_twinsanity/hle/math/culling_planes.h"

#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>

MathLibraryPlanesOracle::Memory::Memory() : bytes(4 * 1024 * 1024) {}
uint8_t* MathLibraryPlanesOracle::Memory::Translate(uint32_t address, size_t size) {
    address &= 0x1fffffffu;
    if (address > bytes.size() || size > bytes.size() - address) std::abort();
    return bytes.data() + address;
}

namespace {
using Oracle = MathLibraryPlanesOracle;
using Math = title::CullingMath;
using Planes = title::CullingPlanes;
unsigned checks = 0, failures = 0;
uint64_t selected_chunk = 0, selected_portal = 0, selected_plane = 0;
void Check(bool value, const char* text) {
    ++checks;
    if (!value && failures++ < 12) std::fprintf(stderr, "%s\n", text);
}
uint32_t Next(uint32_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
float Input(uint32_t& seed, bool raw) {
    const auto bits = Next(seed);
    return raw ? Float(bits) : static_cast<float>(static_cast<int32_t>(bits) / 65536) / 256.0f;
}
Math::Matrix Identity() {
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}
template<class T> void Put(Oracle::Memory& memory, uint32_t address, const T& value) {
    std::memcpy(memory.Translate(address, sizeof(value)), &value, sizeof(value));
}
template<class T> T Read(Oracle::Memory& memory, uint32_t address) {
    T result;
    std::memcpy(&result, memory.Translate(address, sizeof(result)), sizeof(result));
    return result;
}
void Call(Oracle::Memory& memory, CpuContext& context, uint32_t entry,
        uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0) {
    context.cpuRegs.GPR.r[4].UD[0] = a0;
    context.cpuRegs.GPR.r[5].UD[0] = a1;
    context.cpuRegs.GPR.r[6].UD[0] = a2;
    Oracle::Invoke(entry, context, memory);
}
Math::Frustum Control(Oracle::Memory& memory, CpuContext& context, const Planes::Planes& view,
        const Planes::Planes& far, const Math::Matrix& place, const Planes::Planes* portal) {
    Put(memory, 0x3b4580, view); Put(memory, 0x3b45e0, far); Put(memory, 0x100, place);
    const Math::PlaneColumns zero{};
    Put(memory, 0x700, zero);
    if (portal) {
        Put(memory, 0x200, *portal);
        Call(memory, context, 0x200b08, 0x400, 0x200);
        Call(memory, context, 0x18d310, 0x3b4580, 0x100, 0x500);
        Call(memory, context, 0x1fd760, 0x700, 0x500, 0);
    } else {
        Call(memory, context, 0x1eed10, 0x100, 0x400, 0x500);
        Call(memory, context, 0x1fd760, 0x700, 0x500, 0);
        Call(memory, context, 0x1fd760, 0x700, 0x510, 1);
    }
    Call(memory, context, 0x1eedc0, 0x100, 0x600);
    return {Read<Math::PlaneColumns>(memory, 0x400), Read<Math::PlaneColumns>(memory, 0x700),
            Read<Math::PlaneColumns>(memory, 0x600)};
}
void Pins() {
    auto place = Identity();
    place[3] = {3, 4, 5, 1};
    Check(Planes::Transform({1, 0, 0, 10}, place) == Math::Vector{1, 0, 0, 7},
        "plane offset subtracts rotated normal dot translation");
    Planes::Planes view{};
    view[0] = {0, 0, 1, 8}; view[5] = {0, 0, -1, 9};
    Planes::Planes far{};
    far[1] = {1, 0, 0, 50};
    Planes::Planes portal{};
    portal[0] = {99, 98, 97, 96}; portal[5] = {95, 94, 93, 92};
    portal[1] = {Float(0x8000807f), Float(0x80000000), Float(0xffc12345), Float(0x807f)};
    const auto result = Planes::ThroughPortal(view, far, place, portal);
    uint32_t raw = 0;
    std::memcpy(&raw, &result.sides.absolute_normals[0][0], 4);
    Check(raw == 0x807f, "raw portal absolute normal preserves denormal payload");
    std::memcpy(&raw, &result.sides.absolute_normals[2][0], 4);
    Check(raw == 0x7fc12345, "raw portal absolute normal clears only sign of NaN payload");
    Check(std::memcmp(&result.sides.offsets[0], &portal[1][3], 4) == 0,
        "portal offsets are copied raw");
    Check(result.depth.normals[2] == Math::Vector{1, 0, 0, 0} &&
        result.depth.offsets == Math::Vector{3, 0, 0, 0}, "portal uses view near plane and no far-depth plane");
    Check(result.far_sides.normals[0][0] == 1 && result.far_sides.offsets[0] == 47,
        "wider side planes transform independently from portal sides");
}
void Sweep(int mode) {
    Check(std::fesetround(mode) == 0, "set caller rounding mode");
    Oracle::Memory memory;
    uint32_t seed = 0x504c414e;
    for (unsigned trial = 0; trial < 2000; ++trial) {
        CpuContext context{};
        context.cpuRegs.GPR.r[29].UD[0] = 0x3f0000;
        Math::Matrix place;
        Planes::Planes view, far, portal;
        for (auto& row : place) for (auto& lane : row) lane = Input(seed, trial % 3 == 0);
        for (auto* rows : {&view, &far, &portal})
            for (auto& row : *rows) for (auto& lane : row) lane = Input(seed, trial % 3 == 0);
        if (trial % 13 == 0) place = Identity();
        const auto control_chunk = Control(memory, context, view, far, place, nullptr);
        const auto counts_chunk = Oracle::Counts();
        ++selected_chunk;
        const auto selected_chunk_result = Planes::InChunk(view, far, place);
        Check(std::memcmp(&control_chunk, &selected_chunk_result, sizeof(control_chunk)) == 0,
            "ordinary chunk planes differ from actual NEW producers");
        Check(Oracle::Counts() == counts_chunk, "selected ordinary planes execute no generated control");
        const auto control_portal = Control(memory, context, view, far, place, &portal);
        const auto counts_portal = Oracle::Counts();
        ++selected_portal;
        const auto selected_portal_result = Planes::ThroughPortal(view, far, place, portal);
        Check(std::memcmp(&control_portal, &selected_portal_result, sizeof(control_portal)) == 0,
            "portal planes differ from actual NEW producers");
        Check(Oracle::Counts() == counts_portal, "selected portal planes execute no generated control");
        Put(memory, 0x100, place); Put(memory, 0x200, view[trial % 6]);
        Call(memory, context, 0x18d310, 0x200, 0x100, 0x300);
        const auto counts_plane = Oracle::Counts();
        const auto control_plane = Read<Math::Vector>(memory, 0x300);
        ++selected_plane;
        const auto selected_plane_result = Planes::Transform(view[trial % 6], place);
        Check(std::memcmp(&control_plane, &selected_plane_result, 16) == 0,
            "separate plane transform differs from actual NEW body");
        Check(Oracle::Counts() == counts_plane, "selected transform executes no generated control");
        Check(std::fegetround() == mode, "plane builders retain caller rounding without a micro-mode override");
    }
}
}

int main() {
    Pins();
    const int previous = std::fegetround();
    for (int mode : {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) Sweep(mode);
    std::fesetround(previous);
    std::printf("MathLibraryPlanesOracle checks=%u failures=%u selected_chunk=%llu selected_portal=%llu selected_plane=%llu\n",
        checks, failures, static_cast<unsigned long long>(selected_chunk),
        static_cast<unsigned long long>(selected_portal), static_cast<unsigned long long>(selected_plane));
    const auto counts = Oracle::Counts();
    for (size_t index = 0; index < counts.size(); ++index)
        std::printf("actual_generated_body_%zu=%llu\n", index, static_cast<unsigned long long>(counts[index]));
    std::printf("NEW source sha256=%s\n", Oracle::SourceDigest());
    return failures ? 1 : 0;
}
