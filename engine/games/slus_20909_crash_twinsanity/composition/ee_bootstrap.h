#pragma once
// EeBootstrap: the Phase-3 composition root -- owns the wiring of the EE side
// (memory + MMIO sink, ELF image, generated dispatch, kernel objects, syscalls
// and the SDK binding manifest, the emulated time domain and boundary owner,
// INTC + EE timers, the savestate providers, the verify host lanes) and runs
// ONE headless leg: a cold boot from the ELF entry, or `--load-state` restore.
//
// A guest park (sceSif* reached, an unimplemented SDK row, an unrecompiled
// pc, a fatal indirect jump) ends the leg: run.meta gets park= / park_pc= /
// park_tid= / park_detail= / unhandled_mmio=, the lanes are finalized
// (finalized=1) and the process exits 0 -- the park is the leg's result.
#include <string>

namespace ps2r {
class Config;
namespace verify { class VerifyHost; }
}  // namespace ps2r
struct TitleProfile;

class EeBootstrap {
public:
    struct Paths {
        std::string elf;        // host path of the ELF
        std::string title_dir;  // games/<title>/ (data map, patches, manifest, expected hashes)
        std::string exe_dir;    // directory of the running executable (input-file path rule); may be empty
    };
    // Runs the leg to its park or to the guest's return; returns the exit code.
    static int Run(const ps2r::Config& cfg, ps2r::verify::VerifyHost& host, const TitleProfile& title,
                   const Paths& paths);

private:
    // The leg is one chain of phases in construction order (each in its own
    // ee_bootstrap_*.cpp): a phase owns the objects it constructs on its frame,
    // hands later phases pointers through LegState (leg_state.h) and tail-calls
    // the next phase, so destruction runs in reverse construction order exactly
    // as the former single scope did. An early return ends the leg.
    struct LegState;
    static int ArmMemoryAndClocks(LegState& s);   // memory, MMIO, time domain, INTC + timers
    static int ArmGraphicsPipeline(LegState& s);  // GS, DrawIR chain, GIF / VU / VIF / DMAC, MTVU
    static int ArmIopAndMedia(LegState& s);       // MMIO ranges, IOP module table, media, mixer, SIF DMA
    static int ArmBoundaryServices(LegState& s);  // boundary slots 0/4, loader, STREAM, MC, pad, libsdr
    static int ArmPlayMode(LegState& s);          // host input / audio device, verify spans, slot 6, protocol
    static int ArmKernelClocks(LegState& s);      // kernel clocks, display tick, field pump, waits, kernel seams
    static int LoadImageAndMovie(LegState& s);    // ELF image, data map, libdma, Movie stream / decoder
    static int InstallDispatch(LegState& s);      // generated dispatch, SDK manifest, title workarounds
    static int InstallChunkCensus(LegState& s);   // the title's chunk state machine as the load classifier
    static int RegisterSections(LegState& s);     // savestate sections, service restore, capture
    static int CensusServices(LegState& s);       // the leg census: services half
    static int CensusPresentation(LegState& s);   // the leg census: presentation half
    static int ArmParkSink(LegState& s);          // the park sink, session workers / restore / audio
    static int ArmVerifySlot(LegState& s);        // slot 8 (Verify): audio pump, lanes, present
    static int RunLeg(LegState& s);               // --load-state restore and relaunch, or the cold boot
};
