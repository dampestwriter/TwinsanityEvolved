// Vu0Programs -- the title's VU0 macro-program table (rules in the header).
#include "games/slus_20909_crash_twinsanity/vu1_library/vu0_programs.h"

#include "engine/vu/vu0/lib_vu0_sin_cos.h"

namespace {

using Kind = Vu0Registry::Kind;

constexpr uint64_t Pair(uint32_t lower, uint32_t upper) { return (static_cast<uint64_t>(upper) << 32) | lower; }

// The libsin/cos fingerprint = the first two SDK bundles ({lower, upper} words).
constexpr uint64_t kSinCosLo = Pair(LibVu0SinCos::kFingerprint[0][0], LibVu0SinCos::kFingerprint[0][1]);
constexpr uint64_t kSinCosHi = Pair(LibVu0SinCos::kFingerprint[1][0], LibVu0SinCos::kFingerprint[1][1]);

const Vu0Registry::Descriptor kEntries[] = {
    {0, 0x00f0, "Sony libsin/cos polynomial", Kind::Hle, kSinCosLo, kSinCosHi},
    {0, 0x0000, "VU0 batch upload bootstrap (kicked from FUN_002b18f8)", Kind::Unimplemented, 0, 0},
    {0, 0x0500, "VU0 4x4 matrix * vector (skinning transform)", Kind::Unimplemented, 0, 0},
    {0, 0x0398, "VU0 4x4 matrix * 4x4 matrix multiply (transform compose)", Kind::Unimplemented, 0, 0},
    {0, 0x0790, "VU0 4x4 matrix concat (pose compose / streaming)", Kind::Unimplemented, 0x01e329bc8000033cull,
     0x01e330bd8000033cull},
    {0, 0x0548, "VU0 quaternion 2-pose blend", Kind::Unimplemented, 0, 0},
    {0, 0x0668, "VU0 quaternion 4-pose blend", Kind::Unimplemented, 0, 0},
    {0, 0x0818, "VU0 morph delta unpack (12-bit fixed -> float)", Kind::Unimplemented, 0, 0},
    {0, 0x0a70, "VU0 morph delta refinement (optional pass)", Kind::Unimplemented, 0, 0},
    {0, 0x01c0, "VU0 morph pose lerp (alpha blend, finalisation)", Kind::Unimplemented, 0, 0},
    {0, 0x0a08, "VU0 collision/intersection test (path A)", Kind::Unimplemented, 0, 0},
    {0, 0x0b90, "VU0 collision/intersection test (path B)", Kind::Unimplemented, 0, 0},
    {0, 0x0ab8, "VU0 ray/triangle nearest-hit test (TR SetRay / StartRayTriangle / FinishRayTriangle; title::RayMath)", Kind::Unimplemented, 0, 0},
    {0, 0x0cb0, "VU0 entity distance test (streaming/LOD)", Kind::Unimplemented, 0, 0},
    {1, 0x0000, "VU1 render dispatcher (JALR-trampoline at 0x40)", Kind::Interpreted, 0, 0},
};

const Vu0Registry::Descriptor kSubroutines[] = {
    {1, 0x1720, "VU1 per-MSCAL setup pass (loads vf1/vf2 from frame data)", Kind::Interpreted, 0, 0},
};

}  // namespace

void Vu0Programs::Register(Vu0Registry& registry) {
    for (const Vu0Registry::Descriptor& d : kEntries) registry.AddEntry(d);
    for (const Vu0Registry::Descriptor& d : kSubroutines) registry.AddSubroutine(d);
}

const Vu0Registry::Descriptor* Vu0Programs::Entries(size_t& count) {
    count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

const Vu0Registry::Descriptor* Vu0Programs::Subroutines(size_t& count) {
    count = sizeof(kSubroutines) / sizeof(kSubroutines[0]);
    return kSubroutines;
}
