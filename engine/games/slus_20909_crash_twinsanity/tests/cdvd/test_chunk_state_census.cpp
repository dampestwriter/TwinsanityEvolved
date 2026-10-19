// ChunkStateCensus pin test (frame skip: the level-entry
// cut as a LOAD tick). The census wraps the title's ChunkLoaderBase::StepStates
// and StepChunkData bodies (DispatchTable capture + install shape) over a fake
// guest memory, reads the states the title wrote and classifies each step:
//   loader step ending Loading / Unloading   -> ChunkLoading / ChunkUnloading
//   chunk step in Releasing / Released        -> ChunkRelease
//   a chunk's FIRST UNPAUSED Shown step that  -> ChunkCut (once per stay in
//   UPDATES every kind of its instances          Shown -- a chunk released and
//   (instances != 0, steppedKinds == -1,         shown again cuts again; the
//   a1 paused == 0)                              title holds GameStatePaused
//                                                through the load, so the Shown
//                                                steps before the resume are
//                                                not the cut: s9's 1176 vs the
//                                                33 ms cut 1178)
// Both original bodies run unchanged (counted), an interior re-entry (ep !=
// the function start) is not a step, and nothing is written to guest memory.
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/ee/timing/tick_profile.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/chunk_state_census.h"

static int g_fail = 0;
#define CHECK(cond, name) \
    do { if (cond) std::printf("PASS %s\n", name); else { std::printf("FAIL %s\n", name); g_fail++; } } while (0)

namespace {

// The rig: data only (the free functions below drive it).
struct Rig {
    std::unordered_map<uint32_t, uint32_t> mem;   // guest word -> value
    std::map<uint32_t, ChunkStateCensus::Body> table;
    int states_runs = 0, chunk_runs = 0;
    ChunkStateCensus census{[this](uint32_t a) {
        auto it = mem.find(a);
        return it == mem.end() ? 0u : it->second;
    }};
};

void SeedOriginals(Rig& r) {
    r.table[ChunkStateCensus::kStepStates] = [&r](CpuContext&, uint32_t) { ++r.states_runs; };
    r.table[ChunkStateCensus::kStepChunkData] = [&r](CpuContext&, uint32_t) { ++r.chunk_runs; };
}

bool Install(Rig& r) {
    return r.census.InstallHooks(
        [&r](uint32_t a) -> ChunkStateCensus::Body {
            auto it = r.table.find(a);
            return it == r.table.end() ? ChunkStateCensus::Body{} : it->second;
        },
        [&r](uint32_t a, ChunkStateCensus::Body b) {
            r.table[a] = std::move(b);
            return true;
        });
}

void StepStates(Rig& r, uint32_t loader, uint32_t ep = ChunkStateCensus::kStepStates) {
    CpuContext ctx{};
    ctx.cpuRegs.GPR.r[4].UL[0] = loader;
    r.table[ChunkStateCensus::kStepStates](ctx, ep);
}

void StepChunk(Rig& r, uint32_t chunk, uint32_t ep = ChunkStateCensus::kStepChunkData, bool paused = false) {
    CpuContext ctx{};
    ctx.cpuRegs.GPR.r[4].UL[0] = chunk;
    ctx.cpuRegs.GPR.r[5].UL[0] = paused ? 1u : 0u;
    r.table[ChunkStateCensus::kStepChunkData](ctx, ep);
}

void SetChunkState(Rig& r, uint32_t chunk, uint32_t state) {
    r.mem[chunk + ChunkStateCensus::kChunkFlagsOffset] = (state & 0xfu) << ChunkStateCensus::kChunkStateShift;
}

}  // namespace

int main() {
    using TP = ps2r::ee::TickProfile;
    // t1: the loader step classification is the state the step ENDED in.
    CHECK(ChunkStateCensus::LoaderEvents(ChunkStateCensus::Loading) == TP::ChunkLoading, "t1a Loading -> ChunkLoading");
    CHECK(ChunkStateCensus::LoaderEvents(ChunkStateCensus::Unloading) == TP::ChunkUnloading, "t1b Unloading -> ChunkUnloading");
    CHECK(ChunkStateCensus::LoaderEvents(ChunkStateCensus::Loaded) == 0 && ChunkStateCensus::LoaderEvents(ChunkStateCensus::None) == 0 &&
          ChunkStateCensus::LoaderEvents(ChunkStateCensus::Queued) == 0, "t1c Loaded/None/Queued -> no event");

    // t2: the chunk step classification: the first UNPAUSED updating Shown step is the cut, once per stay.
    {
        Rig r;
        const uint32_t c = 0x1000;
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Hidden, false, true) == 0, "t2a Hidden -> no event");
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Shown, false, true) == 0, "t2b Shown, nothing to update, paused -> not the cut");
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Shown, true, true) == 0, "t2c Shown, updating, still PAUSED (the load) -> not the cut");
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Shown, true, false) == TP::ChunkCut, "t2d the first unpaused updating step -> ChunkCut");
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Shown, true, false) == 0, "t2e the next step -> no event");
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Shown, true, true) == 0 &&
              r.census.ChunkEvents(c, ChunkStateCensus::Shown, true, false) == 0, "t2f a pause-menu pause and resume in Shown -> no second cut");
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Releasing, false, false) == TP::ChunkRelease, "t2g Releasing -> ChunkRelease");
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Released, false, false) == TP::ChunkRelease, "t2h Released -> ChunkRelease");
        CHECK(r.census.ChunkEvents(c, ChunkStateCensus::Shown, true, false) == TP::ChunkCut, "t2i shown again after a release -> cut again");
        CHECK(r.census.ChunkEvents(0x2000, ChunkStateCensus::Shown, true, false) == TP::ChunkCut, "t2j another chunk's first live update -> its own cut");
        CHECK(r.census.Cuts() == 3, "t2k cuts counted (3)");
    }

    // t3: the hooks: both bodies wrapped, the originals run, the states read from guest memory.
    {
        Rig r;
        SeedOriginals(r);
        CHECK(Install(r), "t3a InstallHooks succeeds with both bodies present");
        CHECK(r.census.Installed(), "t3b Installed()");
        CHECK(!Install(r), "t3c a second install is refused");
        const uint32_t loader = 0x00400000, chunk = 0x00500000;
        r.mem[loader + ChunkStateCensus::kLoaderStateOffset] = ChunkStateCensus::Loading;
        StepStates(r, loader);
        CHECK(r.states_runs == 1 && r.census.LoaderSteps() == 1, "t3d a whole StepStates step runs the original and counts");
        StepStates(r, loader, ChunkStateCensus::kStepStates + 0x40);
        CHECK(r.states_runs == 2 && r.census.LoaderSteps() == 1, "t3e an interior re-entry runs the original, is not a step");
        SetChunkState(r, chunk, ChunkStateCensus::Hidden);
        StepChunk(r, chunk);
        SetChunkState(r, chunk, ChunkStateCensus::Shown);
        StepChunk(r, chunk);   // Shown, no instances yet (the words read 0): not the cut
        CHECK(r.census.Cuts() == 0, "t3f a Shown step before the instances exist is not the cut");
        r.mem[chunk + ChunkStateCensus::kChunkInstancesOffset] = 0x00600000;
        r.mem[chunk + ChunkStateCensus::kChunkSteppedKindsOffset] = 0xffffffffu;   // EveryKindStepped
        StepChunk(r, chunk, ChunkStateCensus::kStepChunkData, /*paused=*/true);   // the load's paused state
        CHECK(r.census.Cuts() == 0, "t3g a paused updating step (the load) is not the cut");
        StepChunk(r, chunk);
        StepChunk(r, chunk);
        CHECK(r.chunk_runs == 5 && r.census.ChunkSteps() == 5, "t3h StepChunkData steps run the original and count");
        CHECK(r.census.Cuts() == 1, "t3i one cut: the chunk's first unpaused updating Shown step");
        StepChunk(r, chunk, ChunkStateCensus::kStepChunkData + 0x10);
        CHECK(r.chunk_runs == 6 && r.census.ChunkSteps() == 5, "t3j an interior re-entry is not a chunk step");
        CHECK(r.mem.size() == 4, "t3k nothing written to guest memory (four words seeded, four words present)");
    }

    // t4: a missing body refuses the install (nothing wrapped).
    {
        Rig r;
        SeedOriginals(r);
        r.table.erase(ChunkStateCensus::kStepChunkData);
        CHECK(!Install(r) && !r.census.Installed(), "t4 install refused when StepChunkData is absent");
    }

    if (g_fail == 0) std::printf("ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}
