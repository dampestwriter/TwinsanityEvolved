#pragma once
// MetaArena -- HOOK_RECLASSIFICATION section 3.1, row 0x181560, class
// META-ARENA (quarantined title workaround; not a PS2 rule).
//
// OWNS the 2 MB backing store the OLD tree exposed as `meta_arena_memory`
// and registers it on the AddressMap as the extra physical region
// [0x02000000, +2 MB). The game's compacting handle heap (static singleton
// DAT_0031b138) keeps its 0x20-byte block descriptors, the 0x18-byte
// compaction job and the 0x0c-byte deferred-op nodes on the shared slab via
// FUN_00181560(size <= 0x20) called from the pool-allocator code
// [0x00202000, 0x00206000); the hook (free_list_0x181560) routes those
// allocations here and frees arena pointers here, so a freed descriptor's
// memory is never re-vended as game data.
//
// Cell layout (frozen -- the META section and mem.mhash depend on it): one
// 0x40-byte cell per node, payload at cell + kSkew (0x0c); a free tombstones
// the three link words (+0x10/+0x14/+0x18) and clears +0x1c below bit 18;
// freed cells are reused FIFO only once more than kQuarantineMin are held.
//
// Savestate: the arena bytes ride the META section (MemorySections); the
// bookkeeping (state bytes, bump, quarantine, counters) is the HEAP section's
// tail (SerializeState/RestoreState, OLD hle_meta_arena.cpp layout).
// restore.mhash covers the 2 MB bytes as the third span.
//
// Suspected defect (why this exists): a use-after-free in the game's own
// descriptor list maintenance (stale +0x10/+0x14/+0x18 links after a free),
// masked on real hardware by the slab's LIFO reuse pattern.
// Decided against the console: the hardware keeps all
// three node kinds on the title's page pool (PCSX2: 385 descriptors in slab
// class 7/32 B, 12 class-7 pages); the corruption the routing hid was the
// original tree's own. Config `hle.meta_arena` (`off` = the title's allocator
// as recompiled) is the toggle; the arena then allocates nothing but keeps its
// storage and its free routing for images written with the routing on (their
// nodes still live here). With the routing off a fresh run's META section is
// an all-zero 2 MB payload (file layout unchanged) and the HEAP tail is the
// empty arena. RETIREMENT: the row retires when the default is `off` and the
// anchors are re-written without arena nodes; until then the class stays.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class AddressMap;

class MetaArena {
public:
    static constexpr uint32_t kBase = 0x02000000u;
    static constexpr uint32_t kSize = 0x00200000u;
    static constexpr uint32_t kBlock = 0x40u;
    static constexpr uint32_t kSkew = 0x0Cu;
    static constexpr uint32_t kPayloadMax = 0x20u;
    static constexpr uint32_t kCapacity = kSize / kBlock;  // 32768 cells
    static constexpr uint32_t kQuarantineMin = 256u;
    // The hook's routing predicate (title constants; the engine never sees them).
    static constexpr uint32_t kAllocSite = 0x00181560u;
    static constexpr uint32_t kFreeSite = 0x00181590u;
    static constexpr uint32_t kPoolCodeLo = 0x00202000u;
    static constexpr uint32_t kPoolCodeHi = 0x00206000u;

    struct Stats {
        uint64_t allocs = 0, frees = 0, double_frees = 0, invalid_frees = 0;
        int32_t live = 0, live_peak = 0;
        uint32_t quarantined = 0, bump_used = 0;
    };

    MetaArena();
    void Register(AddressMap& map);

    // True when FUN_00181560(size) from `ra` is a routed metadata allocation.
    static bool RoutesAllocation(uint32_t size, uint32_t ra);
    static bool Contains(uint32_t guest_addr);

    // Guest address of a payload (cell + kSkew), or 0 when exhausted.
    uint32_t Alloc();
    void Free(uint32_t guest_addr);

    uint8_t* Data() { return bytes_.data(); }
    const uint8_t* Data() const { return bytes_.data(); }
    size_t Size() const { return bytes_.size(); }
    Stats GetStats() const;

    // HEAP-section tail (OLD layout): capacity, state[capacity], bump,
    // quarantine count + indices, four u64 counters, live, live_peak.
    void SerializeState(std::vector<uint8_t>& out) const;
    bool ValidateState(const uint8_t* p, size_t len, size_t* consumed, std::string* err) const;
    bool RestoreState(const uint8_t* p, size_t len, size_t* consumed);

    void ResetForTest();

private:
    void Tombstone(uint32_t cell_off);

    std::vector<uint8_t> bytes_;
    std::vector<uint8_t> state_;        // 0 never used, 1 live, 2 freed
    std::deque<uint32_t> quarantine_;   // freed cell indices, FIFO
    uint32_t bump_ = 0;
    Stats stats_;
};
