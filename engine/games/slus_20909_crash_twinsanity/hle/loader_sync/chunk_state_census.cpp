#include "games/slus_20909_crash_twinsanity/hle/loader_sync/chunk_state_census.h"

#include <utility>

#include "engine/ee/timing/tick_profile.h"

ChunkStateCensus::ChunkStateCensus(Read32 read) : read_(std::move(read)) {}

uint8_t ChunkStateCensus::LoaderEvents(uint32_t state) {
    if (state == Loading) return ps2r::ee::TickProfile::ChunkLoading;
    if (state == Unloading) return ps2r::ee::TickProfile::ChunkUnloading;
    return 0;
}

uint8_t ChunkStateCensus::ChunkEvents(uint32_t chunk, uint32_t state, bool updates, bool paused) {
    uint8_t events = 0;
    Seen& seen = seen_[chunk];   // a chunk first seen: state 0 (Hidden), armed
    if (state != Shown) seen.armed = true;   // a stay outside Shown re-arms the cut
    if (state == Shown && updates && !paused && seen.armed) {
        // The chunk's first UNPAUSED update with every kind stepped: the cut. The
        // title holds GameStatePaused through the load (StepChunkData's `paused`;
        // the object clock stopped); the chunk went Shown and stepped its
        // instances two ticks before the state resumed on s9 (1176 vs 1178).
        events |= ps2r::ee::TickProfile::ChunkCut;
        seen.armed = false;
        ++cuts_;
    }
    if (state == Releasing || state == Released) events |= ps2r::ee::TickProfile::ChunkRelease;
    seen.state = state;
    if (state == Released && seen_.size() > 4096) seen_.erase(chunk);   // a bound; the next step re-learns it
    return events;
}

void ChunkStateCensus::Note(uint8_t events) {
    for (uint8_t bit = 1; bit != 0 && bit <= ps2r::ee::TickProfile::ChunkCut; bit = static_cast<uint8_t>(bit << 1))
        if (events & bit) ps2r::ee::TickProfile::NoteChunkEvent(static_cast<ps2r::ee::TickProfile::ChunkEvent>(bit));
}

bool ChunkStateCensus::InstallHooks(const Capture& capture, const Install& install) {
    if (installed_ || !capture || !install || !read_) return false;
    Body step_states = capture(kStepStates);
    Body step_chunk = capture(kStepChunkData);
    if (!step_states || !step_chunk) return false;
    const bool a = install(kStepStates, [this, orig = std::move(step_states)](CpuContext& ctx, uint32_t ep) {
        const bool whole = !ep || ep == kStepStates;   // an interior re-entry has no `this` in a0
        const uint32_t self = ctx.cpuRegs.GPR.r[4].UL[0];
        orig(ctx, ep);
        if (!whole) return;
        ++loader_steps_;
        Note(LoaderEvents(read_(self + kLoaderStateOffset)));
    });
    const bool b = install(kStepChunkData, [this, orig = std::move(step_chunk)](CpuContext& ctx, uint32_t ep) {
        const bool whole = !ep || ep == kStepChunkData;
        if (whole) {
            const uint32_t chunk = ctx.cpuRegs.GPR.r[4].UL[0];
            ++chunk_steps_;
            const uint32_t state = (read_(chunk + kChunkFlagsOffset) >> kChunkStateShift) & 0xfu;
            // "Updates" = every kind of its instances steps: the chunk is within the
            // loader's wanted depth (Sm2Loader::Step: steppedKinds = EveryKindStepped),
            // i.e. the focus chunk and the chunks linked to it -- a chunk at the edge
            // depth only animates its models (1 << NodeModel) and is not the cut.
            const bool updates = state == Shown && read_(chunk + kChunkInstancesOffset) != 0 &&
                                 read_(chunk + kChunkSteppedKindsOffset) == kEveryKindStepped;
            Note(ChunkEvents(chunk, state, updates, ctx.cpuRegs.GPR.r[5].UL[0] != 0));
        }
        orig(ctx, ep);
    });
    installed_ = a && b;
    return installed_;
}
