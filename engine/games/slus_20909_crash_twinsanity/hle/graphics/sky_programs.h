#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// The VU1 program RESIDENCY a sky material's model-arm set-up leaves behind,
// reproduced by the owned sky pass byte for byte.
//
// Source (TR renderer/materials.cpp RenderSkyMaterial -> StartsPrograms /
// LoadPrograms, vuprograms.cpp LoadProgramForMaterial / WriteProgramUpload,
// renderer.h BeginPacket / EndPacket; retail SLUS-20909 bodies 0x1bae58,
// 0x1da0f0, 0x1da188): the first material of a bucket whose shader key differs
// from the bucket's last one allocates the bucket's program region
// (g_VuProgramNext = region start, g_VuProgramTime++) and every shader program
// at or above FirstLoadedShaderProgram (0xF) is UPLOADED into VU1 micro memory
// by a CNT tag (FLUSHE, STCYCL) and REF tags (MPG, 240 instructions each,
// straight from the title's own .vutext bytes) appended to the bucket's DMA
// chain, its g_VuPrograms entry taking the address, the time and the bucket's
// slot.
//
// Why the owned pass must do this although it draws no VU1 geometry: the
// title's VU1 library keys a kick on the WHOLE micro image (the per-material
// region's hash). The sky's type-A shader program (0x0A) is the first load of
// the frame; without it the image every later kick of the frame runs under is
// one the corpus never saw -> the worker library misses on ~1.1 M kicks a
// level route, the raw PATH1 route takes them, and play mode runs away in
// memory (allon_w2 REVERTED_2a: peak_mb 1654, Shadow pass limit). The owned
// pass emits exactly the upload and the bookkeeping, never the sky's data
// unpacks, shader packets, buffer flip or CALL (those are the owned draw).
//
// Everything here is software state of the title (its own packet memory, its
// own tables): no device register, no renderer packet of ours, no guest call.
class SkyPrograms {
public:
    struct Write { uint32_t address; std::vector<uint8_t> bytes; };
    static constexpr uint32_t kBuckets = 0x00323658, kBucketStride = 0x38, kBucketCount = 28;
    static constexpr uint32_t kBucketLast = 0x10, kBucketRegion = 0x16, kBucketChain = 0x18;
    static constexpr uint32_t kBucketLastKey = 0x20, kBucketLastCall = 0x34;
    static constexpr uint32_t kChains = 0x00323300, kChainStride = 0x28, kChainCount = 10;
    static constexpr uint32_t kChainNext = 0x10, kChainStart = 0x14, kChainCapacity = 0x18;
    static constexpr uint32_t kPrograms = 0x003b0470, kProgramStride = 0x4c, kProgramCount = 43;
    static constexpr uint32_t kProgramCode = 0, kProgramAddress = 4, kProgramSize = 8;
    static constexpr uint32_t kProgramLoaded = 0xc, kProgramBucketAddresses = 0x12;
    // Region 1's start is taken when the bucket's region byte is 1, region 2's otherwise.
    static constexpr uint32_t kRegion1 = 0x0030a7e8, kRegion2 = 0x0030a7ec, kTime = 0x0030a7f0;
    static constexpr uint32_t kBucket = 0x0030a7f4, kNext = 0x0030a7f8, kStart = 0x0030a7fc;
    static constexpr uint32_t kTypeAProgram = 0x00309a94;   // g_ShaderType0AProgram
    static constexpr uint32_t kTypeAVtable = 0x002fa948;    // SkyMaterial::kVtable
    static constexpr uint64_t kNoProgramsKey = 1;
    static constexpr uint32_t kFirstLoadedProgram = 0xf, kMpgChunk = 0xf0, kMicroInstructions = 2048;
    static constexpr uint32_t kMaterialShaders = 0, kMaterialShaderCount = 0x10, kMaterialKey = 0x18;
    static constexpr uint32_t kMaterialBucket = 0x20, kShaderVtable = 0x6c, kMaxMaterialShaders = 4;
    static constexpr uint32_t kCntTag = 0x10000000, kNextTag = 0x20000000, kRefTag = 0x30000000;
    static constexpr uint32_t kVifFlushE = 0x10000000, kVifCycle1 = 0x01000101, kVifMpg = 0x4a000000;
    enum class Result { Ok, InvalidMemory, UnsupportedState };
    struct Plan {
        std::vector<Write> writes;
        uint32_t uploads = 0;       // MPG program uploads emitted
    };
    // The live residency state at selection: the globals, the bucket's chain
    // cursor and last tag, the program table entries the plan touches. One
    // Capture per owned pass; Step per material in the pass's material order
    // (the plan carries the state forward exactly as the model's writes would).
    bool Capture(const MaterialState::Read& read);
    // The material's complete program side effects as writes (all or none).
    Result Step(const MaterialState::Read& read, uint32_t material, Plan& out);

private:
    struct Bucket { uint32_t last = 0, chain = 0; uint64_t last_key = 0; uint8_t region = 0; bool loaded = false; };
    struct Chain { uint32_t next = 0, start = 0, capacity = 0; bool loaded = false; };
    struct Program { uint32_t code = 0, size = 0, loaded = 0; bool known = false; };
    Result Bucket_(const MaterialState::Read& read, uint32_t index, Bucket*& out);
    Result Chain_(const MaterialState::Read& read, uint32_t index, Chain*& out);
    Result Program_(const MaterialState::Read& read, uint32_t index, Program*& out);
    Result Upload(const MaterialState::Read& read, uint32_t bucket_index, Bucket& bucket, Chain& chain,
                  uint32_t program_index, Program& program, Plan& out);
    uint32_t time_ = 0, next_ = 0, region1_ = 0, region2_ = 0;
    std::array<Bucket, kBucketCount> buckets_{};
    std::array<Chain, kChainCount> chains_{};
    std::array<Program, kProgramCount> programs_{};
    bool captured_ = false;
};

}  // namespace title
