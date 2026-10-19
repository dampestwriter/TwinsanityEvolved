#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_buckets.h"

#include <limits>

namespace title {
namespace {
uint32_t Word(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
}  // namespace

// TR:renderer/renderer.h:402,431 defines writer fields and the leading 8 bytes;
// TR:renderer/buckets.cpp:205 links these writers then reuses their buffers.
// Full source root is TR:src/platform/ps2/, commit
// 38d533d0033b48b5d6b49880c49f5eeeade0f38e.
// NTSC recompiled_shard_008.cpp:20948 matches a0 + 8 and the 0x38 stride:
// +0x40 is bucket 1.first; bucket 0.last is at +0x10.
GraphicsBuckets::Result GraphicsBuckets::Observe(
    uint32_t address, uint32_t from_interrupt, const Reader& reader, Snapshot* output) {
    ++counters_.observed_submissions;
    if (!output || (address & 3) != 0 ||
        address > std::numeric_limits<uint32_t>::max() - (kObjectBytes - 1)) {
        ++counters_.failed_snapshots;
        return Result::InvalidRange;
    }
    std::array<uint8_t, kObjectBytes> bytes{};
    bool read = false;
    try {
        read = reader && reader(address, bytes.data(), bytes.size());
    } catch (...) {
        read = false;
    }
    if (!read) {
        ++counters_.failed_snapshots;
        return Result::Unavailable;
    }
    Snapshot captured;
    captured.address = address;
    captured.from_interrupt = from_interrupt != 0;
    for (size_t index = 0; index < captured.buckets.size(); ++index) {
        const auto* at = bytes.data() + 8 + index * 0x38;
        auto& bucket = captured.buckets[index];
        bucket.first = Word(at);
        bucket.insertion = Word(at + 4);
        bucket.last = Word(at + 8);
        bucket.vu_buffer = at[0x0c];
        bucket.program_region = at[0x0e];
        bucket.chain = Word(at + 0x10);
        bucket.last_program_key = uint64_t(Word(at + 0x18)) |
                                  (uint64_t(Word(at + 0x1c)) << 32);
        bucket.last_2d_material = Word(at + 0x24);
        bucket.last_call = Word(at + 0x2c);
        bucket.last_joints = Word(at + 0x30);
        bucket.last_slot = Word(at + 0x34);
    }
    *output = captured;
    return Result::Captured;
}

GraphicsBuckets::Counters GraphicsBuckets::Counts() const { return counters_; }

}  // namespace title
