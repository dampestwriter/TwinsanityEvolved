// Vu1LoadedPrograms -- the shader-type program catalog (rules in the header).
//
// Hashes and sizes read from the title's executable at the addresses the
// RegisterShaderType* functions hand Register (symbols/pal_ntsc_map.tsv ->
// TR renderer/vuprograms.cpp:323-615); type 0x1B shares type 0x01's code and
// type 0x00 registers no program. Sizes are the upload sizes (bundles * 8,
// even bundle counts).
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_loaded_programs.h"

namespace {

constexpr Vu1LoadedProgram kPrograms[] = {
    {0xb1c5bddf26cf2701ull, 1904, "shader_type_01"},  // 238 bundles (type 0x1B shares it)
    {0xb37a46283257121dull, 1824, "shader_type_02"},  // 228
    {0x1fb5aeefa2875d0full, 688, "shader_type_03"},   // 86
    {0x98e320959b851f47ull, 896, "shader_type_04"},   // 112
    {0xd9c9720b81b93b44ull, 1392, "shader_type_0a"},  // 174
    {0x57f3460eb627b506ull, 1888, "shader_type_0b"},  // 236
    {0xfb3b5e1c81beb9dcull, 3568, "shader_type_0c"},  // 446
    {0xcefee708f2bdc7cdull, 624, "shader_type_0d"},   // 78
    {0x95ad9fe57dce191aull, 1568, "shader_type_0f"},  // 196
    {0x4c019eb3e50f1b4dull, 2704, "shader_type_10"},  // 338
    {0xd518796b50365fbfull, 1056, "shader_type_11"},  // 132
    {0xee9908300182eea6ull, 992, "shader_type_12"},   // 124
    {0x8c9259f98205f849ull, 720, "shader_type_13"},   // 90
    {0x97afd4a2fa1acee8ull, 1536, "shader_type_15"},  // 192
    {0xa81e3ac4b96e0271ull, 3232, "shader_type_16"},  // 404
    {0x1f910f33851ba722ull, 1984, "shader_type_17"},  // 248
    {0x8da31ffda350d398ull, 1696, "shader_type_18"},  // 212
    {0x685309cb97b345c1ull, 1616, "shader_type_19"},  // 202
    {0x171eb01de70d5e8cull, 2000, "shader_type_1a"},  // 250
    {0x20af9470626c3b9bull, 624, "shader_type_1c"},   // 78
    {0x4cc0329bec557017ull, 1232, "shader_type_1e"},  // 154
    {0xed71c2cc1de6a12dull, 2080, "shader_type_1f"},  // 260
    {0xebfb03bf99c5d678ull, 800, "shader_type_20"},   // 100
};

static_assert(sizeof(kPrograms) / sizeof(kPrograms[0]) == Vu1LoadedPrograms::kCount, "catalog rows = Kind count");

}  // namespace

const Vu1LoadedProgram* Vu1LoadedPrograms::Table() { return kPrograms; }

size_t Vu1LoadedPrograms::Count() { return kCount; }

Vu1LoadedPrograms::Kind Vu1LoadedPrograms::At(const Vu1HleInput& in, uint32_t pc) {
    if (in.info == nullptr) return kNone;
    for (const Vu1LoadedAt& at : in.info->layout)
        if (at.pc == pc && at.program < kCount) return static_cast<Kind>(at.program);
    return kNone;
}

uint32_t Vu1LoadedPrograms::SizeBytes(Kind k) { return k < kCount ? kPrograms[k].size_bytes : 0; }
