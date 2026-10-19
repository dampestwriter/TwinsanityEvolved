#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_programs.h"

#include <cstring>

namespace title {
namespace {
using Read = MaterialState::Read;
template<class T> bool Value(const Read& read, uint32_t address, T& out) {
    return read && address && uint64_t(address) + sizeof(T) <= uint64_t{1} << 32 &&
           read(address, &out, sizeof(T));
}
template<class T> SkyPrograms::Write Store(uint32_t address, const T& value) {
    SkyPrograms::Write result{address, std::vector<uint8_t>(sizeof(T))};
    std::memcpy(result.bytes.data(), &value, sizeof(T));
    return result;
}
bool Ram(uint32_t address) { return address && address < 0x02000000; }
bool Quad(uint32_t address) { return Ram(address) && !(address & 15); }
}  // namespace

bool SkyPrograms::Capture(const Read& read) {
    *this = SkyPrograms{};
    if (!Value(read, kTime, time_) || !Value(read, kNext, next_) ||
        !Value(read, kRegion1, region1_) || !Value(read, kRegion2, region2_)) return false;
    captured_ = true;
    return true;
}

SkyPrograms::Result SkyPrograms::Bucket_(const Read& read, uint32_t index, Bucket*& out) {
    if (index >= kBucketCount) return Result::UnsupportedState;
    auto& bucket = buckets_[index];
    if (!bucket.loaded) {
        const uint32_t base = kBuckets + index * kBucketStride;
        int32_t chain = 0;
        if (!Value(read, base + kBucketLast, bucket.last) || !Value(read, base + kBucketRegion, bucket.region) ||
            !Value(read, base + kBucketChain, chain) || !Value(read, base + kBucketLastKey, bucket.last_key))
            return Result::InvalidMemory;
        if (chain < 0 || uint32_t(chain) >= kChainCount || !Quad(bucket.last)) return Result::UnsupportedState;
        bucket.chain = uint32_t(chain);
        bucket.loaded = true;
    }
    out = &bucket;
    return Result::Ok;
}
SkyPrograms::Result SkyPrograms::Chain_(const Read& read, uint32_t index, Chain*& out) {
    auto& chain = chains_[index];
    if (!chain.loaded) {
        const uint32_t base = kChains + index * kChainStride;
        if (!Value(read, base + kChainNext, chain.next) || !Value(read, base + kChainStart, chain.start) ||
            !Value(read, base + kChainCapacity, chain.capacity)) return Result::InvalidMemory;
        if (!Quad(chain.next)) return Result::UnsupportedState;
        chain.loaded = true;
    }
    out = &chain;
    return Result::Ok;
}
SkyPrograms::Result SkyPrograms::Program_(const Read& read, uint32_t index, Program*& out) {
    if (index >= kProgramCount) return Result::UnsupportedState;
    auto& program = programs_[index];
    if (!program.known) {
        const uint32_t base = kPrograms + index * kProgramStride;
        if (!Value(read, base + kProgramCode, program.code) || !Value(read, base + kProgramSize, program.size) ||
            !Value(read, base + kProgramLoaded, program.loaded)) return Result::InvalidMemory;
        // The registration rounds the instruction count up to a pair; the code
        // is a quadword-aligned .vutext array the REF tags address directly.
        if (!Quad(program.code) || !program.size || (program.size & 1) || program.size > kMicroInstructions ||
            uint64_t(program.code) + uint64_t(program.size) * 8 > 0x02000000) return Result::UnsupportedState;
        program.known = true;
    }
    out = &program;
    return Result::Ok;
}

SkyPrograms::Result SkyPrograms::Upload(const Read&, uint32_t bucket_index, Bucket& bucket, Chain& chain,
                                        uint32_t program_index, Program& program, Plan& out) {
    // LoadProgramForMaterial: the entry takes the next address, the time and
    // the bucket's slot; WriteProgramUpload appends the packet; Next advances.
    const uint32_t address = next_;
    if (address + program.size > kMicroInstructions) return Result::UnsupportedState;
    const uint32_t chunks = (program.size + kMpgChunk - 1) / kMpgChunk;
    const uint32_t bytes = 16 + chunks * 16 + 16;   // CNT, REF..., the new NEXT tag
    if (chain.next + bytes > 0x02000000) return Result::UnsupportedState;
    if (chain.start && chain.capacity &&
        (chain.next < chain.start || uint64_t(chain.next) + bytes > uint64_t(chain.start) + uint64_t(chain.capacity) * 16))
        return Result::UnsupportedState;
    const uint32_t entry = kPrograms + program_index * kProgramStride;
    out.writes.push_back(Store(entry + kProgramAddress, address));
    out.writes.push_back(Store(entry + kProgramLoaded, time_));
    out.writes.push_back(Store(entry + kProgramBucketAddresses + bucket_index * 2, uint16_t(address)));
    program.loaded = time_;
    // BeginPacket: the bucket's last tag points at the packet.
    const uint32_t at = chain.next;
    out.writes.push_back(Store(bucket.last + 4, at));
    std::vector<uint8_t> packet(bytes, 0);
    const auto word = [&](uint32_t offset, uint32_t value) { std::memcpy(packet.data() + offset, &value, 4); };
    word(0, kCntTag); word(8, kVifFlushE); word(12, kVifCycle1);
    uint32_t left = program.size, source = program.code, target = address, offset = 16;
    while (left) {
        const uint32_t count = left <= kMpgChunk ? left : kMpgChunk;
        word(offset, (count >> 1) | kRefTag);
        word(offset + 4, source);
        word(offset + 12, target | count << 16 | kVifMpg);
        left -= count; source += count * 8; target += count; offset += 16;
    }
    // EndPacket: a fresh NEXT tag becomes the bucket's last; the chain moves on.
    word(offset, kNextTag);
    out.writes.push_back({at, std::move(packet)});
    bucket.last = at + offset;
    out.writes.push_back(Store(kBuckets + bucket_index * kBucketStride + kBucketLast, bucket.last));
    chain.next = at + offset + 16;
    out.writes.push_back(Store(kChains + bucket.chain * kChainStride + kChainNext, chain.next));
    next_ = address + program.size;
    ++out.uploads;
    return Result::Ok;
}

SkyPrograms::Result SkyPrograms::Step(const Read& read, uint32_t material, Plan& out) {
    if (!captured_) return Result::UnsupportedState;
    uint64_t key = 0;
    uint32_t bucket_index = 0, shader_count = 0;
    if (!Value(read, material + kMaterialKey, key) || !Value(read, material + kMaterialBucket, bucket_index) ||
        !Value(read, material + kMaterialShaderCount, shader_count)) return Result::InvalidMemory;
    if (shader_count > kMaxMaterialShaders) return Result::UnsupportedState;
    Bucket* bucket = nullptr;
    if (const auto result = Bucket_(read, bucket_index, bucket); result != Result::Ok) return result;
    Plan plan;
    // StartsPrograms: keyed like the bucket's last material (or keyed 1) loads
    // nothing; both arms name the bucket as the program bucket.
    const bool loads = key != kNoProgramsKey && key != bucket->last_key;
    if (loads) {
        // The shader programs first (refuse before any write is planned): the
        // owned pass takes type-A shaders only, whose program slot virtual
        // returns g_ShaderType0AProgram.
        std::vector<uint32_t> programs;
        for (uint32_t i = 0; i < shader_count; ++i) {
            uint32_t shader = 0, vtable = 0, program = 0;
            if (!Value(read, material + kMaterialShaders + i * 4, shader) || !Value(read, shader + kShaderVtable, vtable))
                return Result::InvalidMemory;
            if (vtable != kTypeAVtable) return Result::UnsupportedState;
            if (!Value(read, kTypeAProgram, program)) return Result::InvalidMemory;
            if (program < kFirstLoadedProgram) continue;
            programs.push_back(program);
        }
        Chain* chain = nullptr;
        if (const auto result = Chain_(read, bucket->chain, chain); result != Result::Ok) return result;
        bucket->last_key = key;
        plan.writes.push_back(Store(kBuckets + bucket_index * kBucketStride + kBucketLastKey, key));
        const uint32_t start = bucket->region == 1 ? region1_ : region2_;
        if (start >= kMicroInstructions) return Result::UnsupportedState;
        ++time_;
        next_ = start;
        plan.writes.push_back(Store(kTime, time_));
        plan.writes.push_back(Store(kStart, start));
        for (uint32_t index : programs) {
            Program* program = nullptr;
            if (const auto result = Program_(read, index, program); result != Result::Ok) return result;
            if (program->loaded == time_) continue;
            if (const auto result = Upload(read, bucket_index, *bucket, *chain, index, *program, plan);
                result != Result::Ok) return result;
        }
        plan.writes.push_back(Store(kNext, next_));
    }
    plan.writes.push_back(Store(kBucket, bucket_index));
    // RenderSkyMaterial: the bucket's last CALL forgotten (the next material
    // makes its own). The buffer turn (TakeBuffer / WriteNextBuffer) belongs to
    // the sky's own VU1 data, which the owned pass does not send: neither side
    // of that pair is touched, so the EE's and VU1's view of it stay as one.
    plan.writes.push_back(Store(kBuckets + bucket_index * kBucketStride + kBucketLastCall, uint32_t(0)));
    out.uploads += plan.uploads;
    out.writes.insert(out.writes.end(), plan.writes.begin(), plan.writes.end());
    return Result::Ok;
}

}  // namespace title
