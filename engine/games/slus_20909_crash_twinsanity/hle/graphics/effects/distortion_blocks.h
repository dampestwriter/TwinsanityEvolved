#pragma once

#include <map>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// Tracks the game's block-list operations. The drawing path reads only the
// twelve ordinary ParticleRecords in each block, never its transport header.
class DistortionBlocks {
public:
    using Read = MaterialState::Read;
    using Body = std::function<void(CpuContext&, uint32_t)>;
    static constexpr uint32_t kInitialize = 0x1b36b8, kResize = 0x1b3f70, kEvents = 0x1b5cd8;
    static constexpr uint32_t kPool = 0x30a4e4, kCount = 0x3098cc;
    static constexpr uint32_t kSystems = 0x2e7028, kEventWheel = 0x3a0d10, kEventIndex = 0x30a4dc;
    struct Snapshot {
        uint32_t version = 1, pool = 0;
        uint64_t generation = 0;
        std::map<uint32_t, uint32_t> next;
    };
    struct Counters {
        uint64_t initializes = 0, resizes = 0, event_turns = 0, captured_blocks = 0, failures = 0;
    };
    enum class Result { Ok, NotInitialized, InvalidMemory, UnsupportedSource, LimitExceeded, Faulted };
    explicit DistortionBlocks(Read);
    // Whole CPU game operation, exactly once. These game functions also update
    // events, free lists and records; their source behavior is preserved. The
    // observer derives list ownership from those CPU objects, without reading
    // the packet words they formerly wrote for the hardware model.
    void Observe(uint32_t boundary, CpuContext&, uint32_t entry, const Body& original);
    Result Capture(uint32_t first, std::vector<DistortionGeometry::Block>&);
    bool CaptureState(Snapshot&) const;
    static bool Validate(const Snapshot&);
    bool Restore(const Snapshot&);
    bool Busy() const;
    Counters Counts() const;
private:
    struct Runtime {
        uint32_t address = 0;
        bool hexagons = false;
        int16_t count = 0, wanted = 0, maximum = 0, system = 0;
        std::array<uint32_t, 32> blocks{};
    };
    struct EventPlan {
        Snapshot state;
        std::map<uint32_t, Runtime> final_runtimes;
    };
    Read read_;
    Snapshot state_;
    Counters counts_;
    bool busy_ = false, faulted_ = false;
    Runtime RuntimeAt(uint32_t) const;
    Snapshot Initialized() const;
    Snapshot Resized(const Runtime&, const Runtime&) const;
    EventPlan Events() const;
    void VerifyEvents(const EventPlan&) const;
    static void Chain(Snapshot&, const Runtime&);
    [[noreturn]] void Fail(const char*);
};

}  // namespace title
