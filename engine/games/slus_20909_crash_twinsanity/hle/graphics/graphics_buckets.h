#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics.h"

namespace title {

// Read-only title-entry observation. A snapshot describes writer boundaries,
// not a decoded or owned packet chain. It cannot authorize native cutover.
class GraphicsBuckets {
public:
    static constexpr uint32_t kSubmitAddress = 0x0017ed50;
    static constexpr uint32_t kStripAddress = 0x001a9120;
    static constexpr size_t kObjectBytes = 0x628;
    enum class Result { Captured, InvalidRange, Unavailable };
    using Reader = std::function<bool(uint32_t, uint8_t*, size_t)>;

    struct Bucket {
        uint32_t first = 0;
        uint32_t insertion = 0;
        uint32_t last = 0;
        uint32_t chain = 0;
        uint8_t vu_buffer = 0;
        uint8_t program_region = 0;
        uint64_t last_program_key = 0;
        uint32_t last_2d_material = 0;
        uint32_t last_call = 0;
        uint32_t last_joints = 0;
        uint32_t last_slot = 0;
    };
    struct Snapshot {
        uint32_t address = 0;
        bool from_interrupt = false;
        std::array<Bucket, Graphics::kBucketCount> buckets;
    };
    struct Counters {
        uint64_t observed_submissions = 0;
        uint64_t failed_snapshots = 0;
    };

    // Call BEFORE the generated kSubmitAddress body, using a0 and a1.
    // Always leave the original body/fallback active until producers and a
    // safe ordered span adapter own the complete continuation. No guest writes,
    // MMIO, chain traversal, emission, waiting or retirement credit occur here.
    // On failure, output is unchanged. Address zero is a valid readable range.
    Result Observe(uint32_t address, uint32_t from_interrupt, const Reader& reader,
                   Snapshot* output);
    Counters Counts() const;

private:
    Counters counters_;
};

}  // namespace title
