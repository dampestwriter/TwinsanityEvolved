#pragma once
// ChunkStateCensus -- the TITLE'S chunk state machine as the load classifier
// (FRAME SKIP: the level-entry cut).
//
// What the title does (TR twinsanity-reversed, game/chunkloading.cpp and
// game/chunkdata.cpp; SLUS_209.09 addresses from symbols/pal_ntsc_map.tsv):
//   * ChunkLoaderBase::StepStates (PAL FUN_002aa8a0 = NTSC FUN_002aa020) moves
//     a chunk's RM2/SM2 file loader through None, Queued, WaitingToLoad,
//     LOADING, Loaded, WaitingToUnload, UNLOADING, Unloaded (`state` at +0x10,
//     written at the end of the step). Loading and Unloading are the title's
//     load phase whether or not a file window moved bytes on the tick.
//   * StepChunkData (PAL FUN_001edbd8 = NTSC FUN_001ed448; a0 = ChunkData)
//     steps a chunk by ChunkData::flags.state (bits 18..21 of the word at
//     +0x120: Hidden 0, Shown 1, Releasing 2, Released 3). A chunk goes Shown
//     once both its files are loaded (ChunkLoaderStep); a Shown step runs
//     `instances->Update(clock, clocks, steppedKinds)` (every kind within the
//     loader's wanted depth: steppedKinds +0x158 == EveryKindStepped, the
//     instances +0x15c). The title holds GameStatePaused through the load
//     (`paused` = a1; the object clock stopped: TimeClock.running 0), so the
//     chunk's first Shown steps do nothing visible; its first UNPAUSED step
//     runs every instance's first live update -- the state applied, the
//     script run, the crates' ground raycasts (CrateAgent::CheckGround ->
//     LineOfSight -> CheckCollisionRayCastFast, 11 ms) -- the CUT: s9 tick
//     1178, 33 ms of title code that no file window covers (the chunk went
//     Shown at 1176, the state resumed at 1178). Releasing / Released steps
//     are the previous chunk's teardown in steps.
// The census wraps both bodies (DispatchTable capture + install, the
// FmvAbortHold shape), reads the states, and notes TickProfile::ChunkEvent per
// step; BoundaryProtocol derives the load windows from these events plus the
// loader's, with no tail. The original bodies run unchanged; nothing is written
// to guest memory; the deterministic lanes are unchanged by construction (a
// read and a count on the game thread).
#include <cstdint>
#include <functional>
#include <unordered_map>

#include "engine/ee/cpu/cpu_state.h"

class ChunkStateCensus {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t addr)>;
    using Install = std::function<bool(uint32_t addr, Body body)>;
    using Read32 = std::function<uint32_t(uint32_t addr)>;

    // Title data (SLUS_209.09).
    static constexpr uint32_t kStepStates = 0x002aa020u;     // ChunkLoaderBase::StepStates(this=a0, now=a1)
    static constexpr uint32_t kStepChunkData = 0x001ed448u;  // StepChunkData(chunk=a0, paused=a1, clock=a2)
    static constexpr uint32_t kLoaderStateOffset = 0x10u;    // ChunkLoaderBase::state (s32)
    static constexpr uint32_t kChunkFlagsOffset = 0x120u;    // ChunkData::flags (u32)
    static constexpr uint32_t kChunkStateShift = 18u;        // ChunkFlags::state : 4 after drawStamp:16, hasSky, hasLights
    static constexpr uint32_t kChunkSteppedKindsOffset = 0x158u;   // ChunkData::steppedKinds (s32; 0 = nothing stepped)
    static constexpr uint32_t kChunkInstancesOffset = 0x15cu;      // ChunkData::instances (ChunkInstances*; null = none yet)
    static constexpr uint32_t kEveryKindStepped = 0xffffffffu;    // ChunkData::EveryKindStepped (-1): within the wanted depth
    enum LoaderState : uint32_t { None = 0, Queued, WaitingToLoad, Loading, Loaded, WaitingToUnload, Unloading, Unloaded };
    enum ChunkState : uint32_t { Hidden = 0, Shown = 1, Releasing = 2, Released = 3 };

    explicit ChunkStateCensus(Read32 read);

    // Wraps both generated bodies; returns whether both installs succeeded.
    bool InstallHooks(const Capture& capture, const Install& install);
    bool Installed() const { return installed_; }

    // The classification of one step (exposed for the unit test): the events
    // noted for a loader step that ended in `state`, and for a chunk step in
    // `state` -- `updates` = the step updates every kind of the chunk's
    // instances (it has instances, steppedKinds == EveryKindStepped), `paused`
    // = StepChunkData's a1 (GameStatePaused: held through the load) -- given
    // what the chunk was observed in before: the first UNPAUSED updating Shown
    // step since the chunk was last seen outside Shown is the cut.
    static uint8_t LoaderEvents(uint32_t state);
    uint8_t ChunkEvents(uint32_t chunk, uint32_t state, bool updates, bool paused);

    uint64_t LoaderSteps() const { return loader_steps_; }
    uint64_t ChunkSteps() const { return chunk_steps_; }
    uint64_t Cuts() const { return cuts_; }

private:
    void Note(uint8_t events);
    Read32 read_;
    bool installed_ = false;
    uint64_t loader_steps_ = 0, chunk_steps_ = 0, cuts_ = 0;
    // Per ChunkData (by guest address): the last state it was stepped in and
    // whether its cut is armed (re-armed by a stay outside Shown; fired once).
    struct Seen { uint32_t state = 0; bool armed = true; };
    std::unordered_map<uint32_t, Seen> seen_;
};
