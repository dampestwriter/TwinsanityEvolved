#pragma once
// KeyRedirect -- HOOK_RECLASSIFICATION section 3.8, the KEY-REDIRECT row
// (EMULATION-BUG-WORKAROUND, the one mutator of the original's write gauntlet,
// OLD memory.cpp ~3505-3590). Quarantined title-side.
//
// What it repairs: the level-load object (*0x309508) keeps its lookup key --
// the data pointer at obj+0x500 to "Levels\Earth\Hub\Beach" -- which host-side
// corruption clobbers ("reads as 'd' by the time the manager list populates",
// so the by-name lookup fails forever and the case-6 escape never fires).
// Every 256th guest write (NoteWrite), once the key has been seen pointing at
// the beach string: if the key no longer points at the stable global beach
// string (*0x30bb00) and is not any "levels\" path, the POINTER is rewritten to
// the global one (the string bytes are never touched).
//
// Suspected defect: the same host-induced allocator / overflow corruption
// family as the 0x181560 / 0x2ad110 rows -- PCSX2 keeps the key stable with no
// repair. Root fix: eliminate the corrupting writes (boot double-init leaks +
// loader delivery divergence).
// RETIREMENT: the boot/level legs match OLD's baselines with the scavenger
// disabled AND the key-watch probe never observes a garbage key.
//
// Binding: the composition root calls NoteWrite() from the write path it
// chooses (the original's cadence counted every guest write); Scan() is the
// repair itself and is exact regardless of cadence.
#include <cstdint>

class MainMemory;

class KeyRedirect {
public:
    static constexpr uint32_t kLevelObjectPtr = 0x00309508u;  // *ptr = level-load object
    static constexpr uint32_t kGlobalBeachPtr = 0x0030BB00u;  // *ptr = stable "Levels\Earth\Hub\Beach"
    static constexpr uint32_t kKeyOffset = 0x500u;
    static constexpr uint32_t kCadence = 256u;

    struct Stats {
        uint64_t writes = 0, scans = 0, repairs = 0;
        bool seen_beach = false;
    };

    explicit KeyRedirect(MainMemory& ram);

    void NoteWrite();  // every kCadence-th call runs Scan()
    bool Scan();       // true when the key pointer was rewritten
    const Stats& GetStats() const { return stats_; }

private:
    bool ReadWord(uint32_t addr, uint32_t* out) const;
    bool IsBeachAt(uint32_t addr) const;
    bool IsLevelsPathAt(uint32_t addr) const;

    MainMemory& ram_;
    Stats stats_;
};
