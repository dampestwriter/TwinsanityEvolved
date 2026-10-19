#pragma once
#include "games/slus_20909_crash_twinsanity/composition/ee_bootstrap.h"
#include "app/host/iop_arm.h"

#include "games/slus_20909_crash_twinsanity/composition/graphics_application.h"
#include "games/slus_20909_crash_twinsanity/composition/effects_application.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/scene_state.h"
#include "app/host/checkpoint_capture.h"
#include "app/host/leg_closer.h"
#include "app/host/mtvu_stack.h"
#include "games/slus_20909_crash_twinsanity/composition/startup_ownership.h"
#include "games/slus_20909_crash_twinsanity/composition/native_startup.h"
#include "app/host/platform_input.h"
#include "app/host/session_runtime.h"
#include "engine/gs/hle/present/aspect_selection.h"
#include "games/slus_20909_crash_twinsanity/composition/session_capture.h"
#include "games/slus_20909_crash_twinsanity/composition/savestate_trigger.h"
#include "app/host/display_sequence.h"
#include "games/slus_20909_crash_twinsanity/composition/movie_checkpoint.h"
#include "games/slus_20909_crash_twinsanity/composition/movie_boot.h"
#include "games/slus_20909_crash_twinsanity/composition/movie_sound.h"
#include "app/host/session_clock.h"
#include "app/host/session_files.h"
#include "app/host/session_workers.h"
#include "app/host/session_restore.h"
#include "app/host/session_audio.h"
#include "app/host/clock_census.h"
#include "games/slus_20909_crash_twinsanity/composition/display_rows.h"
#include "games/slus_20909_crash_twinsanity/composition/input_save.h"
#include "games/slus_20909_crash_twinsanity/composition/input_sample.h"
#include "games/slus_20909_crash_twinsanity/composition/video_census.h"
#include "games/slus_20909_crash_twinsanity/composition/frame_census.h"
#include "games/slus_20909_crash_twinsanity/composition/sound_output.h"
#include "games/slus_20909_crash_twinsanity/composition/service_timeline.h"
#include "games/slus_20909_crash_twinsanity/composition/service_import.h"
#include "games/slus_20909_crash_twinsanity/composition/service_restore.h"
#include "app/host/session_clock.h"
#include "games/slus_20909_crash_twinsanity/composition/session_services.h"
#include "games/slus_20909_crash_twinsanity/composition/file_services.h"
#include "games/slus_20909_crash_twinsanity/composition/movie_storage.h"
#include "games/slus_20909_crash_twinsanity/composition/service_trace_log.h"
#include "app/host/subsystem_census.h"
#include "app/host/tick_thread.h"
#include "games/slus_20909_crash_twinsanity/composition/menu_composition.h"
#include "app/host/user_data.h"
#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include "engine/core/config/config.h"
#include "engine/core/config/config_loader.h"
#include "engine/core/config/settings_file.h"
#if PS2R_DIAGNOSTICS
#include "engine/core/diag/diag_registry.h"
#include "engine/core/diag/drawlist_recorder.h"
#include "engine/core/diag/word_watch.h"
#include "engine/vu/vu0/vu0_trace.h"
#endif
#include "engine/core/log/log.h"
#include "engine/core/triage/triage_bundle.h"
#include "engine/gs/hle/ir/drawlist_ring.h"
#include "engine/core/log/logger.h"
#include "engine/core/log/ring_log.h"
#include "engine/core/verify/verify_host.h"
#include "engine/ee/cpu/cpu_context_init.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/cpu/guest_park.h"
#include "engine/ee/hle/alarms.h"
#include "engine/ee/hle/boot_env.h"
#include "engine/ee/hle/event_flags.h"
#include "engine/ee/hle/sdk_binding_manifest.h"
#include "engine/ee/hle/semaphores.h"
#include "engine/ee/hle/syscall_table.h"
#include "engine/ee/hle/thread_manager.h"
#include "engine/ee/intc/ee_timers.h"
#include "engine/ee/intc/intc.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/data_map_applier.h"
#include "engine/ee/mem/elf_loader.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/ee/savestate/intc_section.h"
#include "engine/ee/savestate/kernel_sections.h"
#include "engine/ee/savestate/heap_section.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/ee/savestate/thread_relauncher.h"
#include "engine/ee/savestate/thread_section.h"
#include "engine/ee/savestate/timer_section.h"
#include "engine/ee/sched/ee_event_queue.h"
#include "engine/ee/sched/ee_run_token.h"
#include "engine/ee/timing/boundary_protocol.h"
#include "engine/gs/hle/present/present_policy.h"
#include "engine/ee/timing/boundary_sequencer.h"
#include "engine/ee/timing/game_boundary.h"
#include "engine/ee/timing/derived_clocks.h"
#include "engine/ee/hle/kernl_timer.h"
#include "engine/dmac/dmac.h"
#include "engine/dmac/dmac_channel.h"
#include "engine/dmac/hle/libdma_bindings.h"
#include "engine/ipu/ipu_settings.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/fmv_sector_source_adapter.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "engine/ipu/hle/libmpeg_bindings.h"
#if PS2R_IOP_ORACLE
// The IPU command/DMA model (ledger L7): the ORACLE binary only.
#include "engine/ipu/ipu_device.h"
#include "engine/ipu/hle/dmac_channel_face_adapter.h"
#include "engine/ipu/hle/libipu_api.h"
#include "engine/ipu/hle/libipu_bindings.h"
#else
namespace ps2r::ipu { class LibipuBindings; }   // named by the row binder's signature; never defined here
#endif
#include "engine/ipu/hle/mpeg_api.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/fmv_context_layout_data.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_stream.h"
#include "games/slus_20909_crash_twinsanity/hle/glue/headroom_query_glue.h"
#include "engine/gif/gif_arbiter.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/gif_regs.h"
#include "engine/gif/savestate/gifs_section.h"
#include "engine/gs/hle/ir/gsdl_format.h"
#include "engine/gs/hle/ir/register_op_stream.h"
#include "engine/gif/hle/gs_sink_fanout.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/hle/classrdr/class_classifier.h"
#include "engine/gs/hle/classrdr/class_registry.h"
#include "engine/gs/hle/ir/counting_draw_ir_sink.h"
#include "engine/gs/hle/present/crtc_classifier.h"
#include "engine/gs/hle/queue/draw_ir_queue_consumer.h"
#include "engine/gs/hle/queue/draw_ir_thread_queue.h"
#include "engine/gs/savestate/gs_sections.h"
#include "engine/gs/state/gs_mmio_handler.h"
#include "engine/gs/state/gs_state.h"
#include "engine/vif/vif.h"
#include "engine/vif/vif_mmio_gate.h"
#include "engine/vif/savestate/vifs_section.h"
#include "engine/vu/hle/vu1_hle_runtime.h"
#include "engine/vu/macro/vu_macro_bridge.h"
#include "engine/vu/tiers/vu_tier_dispatch.h"
#include "engine/vu/vu0/vu0_registry.h"
#include "engine/vu/vu_unit.h"
#include "engine/vu/savestate/vu_memory_section.h"
#include "engine/vu/savestate/vu1_register_section.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu0_programs.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/vu1_library.h"
#include "engine/cdvd/hle/libcdvd_bindings.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/read_engine.h"
#include "engine/cdvd/stream_reader.h"
#include "engine/mc/hle/libmc_bindings.h"
#include "engine/mc/hle/mc_command_model.h"
#include "engine/mc/hle/mc_host_store.h"
#include "engine/mc/savestate/mchl_section.h"
#include "engine/pad/hle/libpad_bindings.h"
#include "engine/pad/hle/pad_protocol.h"
#include "engine/pad/hle/pad_stamp_duty.h"
#include "engine/pad/hle/padman_module.h"
#include "engine/pad/savestate/pads_section.h"
#include "engine/core/triage/input_log.h"
#include "engine/mc/hle/mcserv_module.h"
#include "engine/ee/timing/emu_domain.h"
#include "engine/ee/timing/frame_rendezvous.h"
#if PS2R_IOP_ORACLE
#include "engine/sif/hle/ee_rpc_server.h"
#include "engine/sif/hle/main_memory_ee_ram_sink.h"
#endif
#include "engine/sif/hle/rpc_router.h"
#include "engine/platform/platform.h"
#include "engine/platform/sdl/audio_pull_ring.h"
#include "engine/platform/sdl/sdl_audio_sink.h"
#include "engine/sound/fmv_pcm_input.h"
#include "engine/sound/mixer.h"
#include "engine/sound/sound_state_codec.h"
#include "engine/sound/spu_core.h"
#include "engine/sound/hle/libsdr_bindings.h"
#include "engine/sound/hle/sd_block_trans.h"
#include "engine/sound/hle/sdr_remote.h"
#include "engine/core/verify/audio_hasher.h"
#include "engine/core/verify/memory_checkpointer.h"
#include "engine/sif/hle/sce_sif.h"
#include "engine/sif/sif_state.h"
#include "engine/ee/timing/tick_source.h"
#include "engine/ee/timing/tick_sync.h"
#include "engine/gs/hle/sce/gs_sync.h"
#include "engine/gs/hle/sce/libgraph_bindings.h"
#if PS2R_IOP_ORACLE
#include "engine/iop/iop_memory.h"
#include "engine/iop/iop_module_table.h"
#include "engine/iop/savestate/iopm_section.h"
#endif
#include "engine/core/hash/hash.h"
#include "engine/core/savestate/quiescence.h"
#include "engine/core/savestate/savestate_format.h"
#if PS2R_IOP_ORACLE
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/module_layout.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_irx_plugin.h"
#include "games/slus_20909_crash_twinsanity/iop/stream_irx/stream_rpc_server.h"
#endif
#include "games/slus_20909_crash_twinsanity/hle/pad_readiness_hook.h"
#include "games/slus_20909_crash_twinsanity/hle/sif_init_statics.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_spin_lift.h"
#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/ee/cpu/ps2_float.h"
#include "engine/title/title_profile.h"
#include "games/slus_20909_crash_twinsanity/hle/game_libraries.h"
#include "games/slus_20909_crash_twinsanity/composition/fixes_composition.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_extension.h"
#include "games/slus_20909_crash_twinsanity/hle/qol/cutscene_fast_forward.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/fmv_abort_hold.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/chunk_state_census.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/free_list_0x181560.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"

// Private to the composition root's phase TUs (ee_bootstrap*.cpp): the headers the
// phases share, the root's helpers and the state one phase hands the next.

#if PS2R_IOP_ORACLE
using ps2r::iop::IopModuleTable;
#endif
using ps2r::log::Logger;
using ps2r::ringlog::RingLog;

constexpr int kMainThreadPriority = 0;                  // KernlBoot adopts tid 1 at priority 0
// The Phase-3 stub's native present size (the GS's display rectangle owns this
// from Phase 6 on).
constexpr int kPresentW = 640;
constexpr int kPresentH = 448;
// Host footprint census (run.meta footprint=): the physical footprint at the
// 60th boundary (the warmed-up reference), the last boundary and its peak,
// plus the recorder's retained bytes. A footprint that keeps growing with the
// frame count names a host container that retains per-frame work.
constexpr uint64_t kFootprintSeriesEvery = 60;   // one sample per 60 boundaries (the mem lane's cadence)
constexpr size_t kFootprintSeriesMax = 128;      // 7680 boundaries; longer legs keep the first 128
struct FootprintCensus {
    uint64_t b60_bytes = 0, last_bytes = 0, peak_bytes = 0;
    std::string series;   // "b60:79,b120:131,..." in MiB
    size_t series_n = 0;
};

std::string Hex(uint64_t v);
std::string Basename(const std::string& path);
// verify/restore_mhash_expected.tsv: "<anchor file>\t<hash>" per line.
bool ExpectedHash(const std::string& tsv, const std::string& anchor_file, uint64_t* out);
// The process's physical footprint in bytes (platform::host::PhysFootprintBytes -- what the OS charges this task:
// resident private + compressed + swapped), as `vmmap --summary` reports it.
// 0 where the platform offers no cheap query. A host census only: the leg's
// footprint must stay bounded over any number of frames (run.meta footprint=).
uint64_t HostPhysFootprintBytes();
// The CPU's SIMD tiers as the runtime dispatch saw them (platform::host::CpuFeaturesCensus; run.meta cpu_features=).
std::string HostCpuFeaturesCensus();

#if PS2R_DIAGNOSTICS
// --watch-flush-from <boundary>: from that boundary on, the --watch-word census
// is rewritten to <leg>/watch.log every 64 recorded stores, so a leg that
// spins inside one frame (and never reaches the end-of-leg flush) still leaves
// its writers on disk. --watch-record-from <boundary>: stores before that
// boundary are not recorded at all (a hot word cannot fill the census cap before
// the frame of interest). Both off (0) by default; diagnostics builds only.
// `census` writes the leg census (run.meta's text) to <leg>/watch_census.meta at
// every watch flush. One process-wide record: FastMem's write observer is a plain
// function pointer and reads it.
struct WatchFlushState {
    std::string flush_path;
    uint64_t flush_from = 0;
    bool flush_on = false;
    uint64_t record_from = 0;
    bool record_on = true;
    std::function<void()> census;
};
WatchFlushState& WatchFlush();

// The texture-source composition trace (TexresTrace; diagnostics builds only): a
// file <dump-frames dir>/texres_trace.cfg with lines `tbp=0x<hex>`, `cbp=0x<hex>`,
// `bytes=1`, `from=<boundary>` filters the resolver's trace to the reads of that
// TEX0.TBP / CLUT base, keeps their window + CLUT bytes, and from boundary `from`
// on writes each dumped frame's record as <dir>/texres_f<key>.txt (+ the bytes
// under <dir>/texres_f<key>/). A census of what a decode consumed; no CLI flag of
// its own (the CLI parser is not this harness's), no Config key.
struct TexresTraceRequest {
    bool enabled = false;
    ps2r::gs::TexresTrace::Filter filter;
    uint64_t from = 0;
};
TexresTraceRequest ReadTexresTraceRequest(const std::string& dump_dir);
// The last completed frame's trace next to the frame image it belongs to.
void WriteTexresTrace(const ps2r::gs::TexresTrace& tr, const std::string& dump_dir, uint64_t key);
#endif

// What one phase of the leg hands the next: pointers to the objects the earlier
// phases own on their stack frames (every pointer is set before the phase that
// unpacks it runs; a phase that returns early never calls the next one). Lambdas
// that cross a phase are carried as std::function.
struct EeBootstrap::LegState {
    const ps2r::Config* cfg = nullptr;
    ps2r::verify::VerifyHost* host = nullptr;
    const TitleProfile* title = nullptr;
    const EeBootstrap::Paths* paths = nullptr;
    const bool* play_mode = nullptr;
    MainMemory* ram = nullptr;
    Scratchpad* spad = nullptr;
    AddressMap* map = nullptr;
    MetaArena* arena = nullptr;
    MmioSink* sink = nullptr;
    MmioRouter* mmio = nullptr;
    ps2r::app::SessionFiles* session_files = nullptr;
    const std::string* dump_frames_dir = nullptr;
    uint64_t* dump_frames_written = nullptr;
#if PS2R_DIAGNOSTICS
    ps2r::diag::WordWatch* word_watch = nullptr;
    Vu0Trace* vu0_trace = nullptr;
#endif
    Intc* intc = nullptr;
    EmuDomain* domain = nullptr;
    DerivedClocks* clocks = nullptr;
    EeTimers* timers = nullptr;
    SifState* sif_state = nullptr;
    TickSource* tick_source = nullptr;
    FrameRendezvous* rendezvous = nullptr;
    std::atomic<bool>* running = nullptr;
    ps2r::gs::GsState* gs_state = nullptr;
    ps2r::app::VideoStack* video = nullptr;
    const bool* video_live = nullptr;
#if PS2R_DIAGNOSTICS
    const TexresTraceRequest* texres_req = nullptr;
#endif
    std::unique_ptr<ps2r::gs::DrawIrQueueConsumer>* consumer = nullptr;
    std::unique_ptr<ps2r::gs::DrawIrThreadQueue>* gs_queue = nullptr;
    // The pipelined queue's consumer-side VramImage (null = one shared image).
    std::unique_ptr<ps2r::gs::VramImage>* pipeline_vram = nullptr;
#if PS2R_DIAGNOSTICS
    ps2r::diag::DrawlistRecorder* drawlist_recorder = nullptr;
#endif
    ps2r::gs::CountingDrawIrSink* draw_sink = nullptr;
    ps2r::gs::DrawlistRing* drawlist_ring = nullptr;
    GifRegs* gif_regs = nullptr;
    RegisterOpStream* gs_ops = nullptr;
    bool gsops_recorder = false;   // the recorder observes the GIF stream (deterministic profile)
    GsStateSink* gs_state_sink = nullptr;
    GifParser* gif_parser = nullptr;
    VuUnit* vu0 = nullptr;
    VuUnit* vu1 = nullptr;
    KickLowering* kick_lowering = nullptr;
    KickLowering* worker_kick_lowering = nullptr;   // the MTVU worker unit's lowering
    Vu1HleRuntime* vu1_hle = nullptr;
    Vu1HleRuntime* worker_vu1_hle = nullptr;   // the MTVU worker unit's library consult
    Vif* vif = nullptr;
    Dmac* dmac = nullptr;
    ps2r::app::GraphicsOutput* graphics_output = nullptr;
    ps2r::app::EffectsApplication* effects_application = nullptr;
    std::unique_ptr<MtvuStack>* mtvu = nullptr;
    ps2r::gs::GsMmioHandler* gs_mmio = nullptr;
#if PS2R_IOP_ORACLE
    ps2r::ipu::IpuDevice* ipu = nullptr;                          // L7: the oracle binary only
    ps2r::ipu::DmacChannelFaceAdapter* ipu_dmac_face = nullptr;
    ps2r::ipu::LibipuApi* libipu = nullptr;
#endif
    VifMmioGate* vif_gate = nullptr;
    uint64_t* dmac_int1_raises = nullptr;
    uint64_t* dmac_sif_kicks = nullptr;
    uint64_t* dmac_tail_chases = nullptr;
    uint64_t* dmac_tail_chases_parked = nullptr;
    uint64_t* gs_syncpath_flushes = nullptr;
    uint64_t* gs_syncpath_parked = nullptr;
#if PS2R_IOP_ORACLE
    IopModuleTable* iop_table = nullptr;
    ps2r::iop::IopMemory* iop_mem = nullptr;
    MainMemoryEeRamSink* ee_ram_sink = nullptr;
#endif
    IsoFs* iso_fs = nullptr;
    const bool* iso_open = nullptr;
    ReadEngine* read_engine = nullptr;
    StreamReader* stream_reader = nullptr;
    ps2r::sound::SpuCore* spu_core = nullptr;
    ps2r::app::SoundOutput* sound_output = nullptr;
    ps2r::sound::Mixer* mixer = nullptr;
#if PS2R_IOP_ORACLE
    ps2r::title::stream_irx::StreamIrxPlugin* stream_irx = nullptr;
    ps2r::title::stream_irx::StreamIrxPlugin::BootReport* irx_boot = nullptr;
    uint64_t* sif_dma_clamped = nullptr;
#endif
    // Whether the IOP kernel booted STREAM.IRX this leg (the oracle arm's
    // BootReport::entry_ran; always false in the play binary, where no IOP exists).
    const bool* iop_booted = nullptr;
    ps2r::app::NativeStartup* native_startup = nullptr;
    BoundarySequencer* sequencer = nullptr;
    GameBoundary* game_boundary = nullptr;
    uint64_t* gs_frame_boundaries = nullptr;
    uint64_t* gs_irq_raises = nullptr;
    DeliveryMeter* delivery_meter = nullptr;
    LoaderModel* loader_model = nullptr;
    ps2r::app::ServiceTimeline* service_timeline = nullptr;
    std::ofstream* loader_requests = nullptr;
    RpcRouter* rpc_router = nullptr;
#if PS2R_IOP_ORACLE
    ps2r::title::stream_irx::StreamRpcServer* stream_server = nullptr;
#endif
    McHostStore* mc_store = nullptr;
    McCommandModel* mc_model = nullptr;
    McservModule* mcserv = nullptr;
    PadProtocol* pad_protocol = nullptr;
    std::function<uint64_t()>* pad_now = nullptr;
    PadmanModule* padman = nullptr;
    ps2r::sound::hle::SdBlockTrans* sd_block_trans = nullptr;
    ps2r::sound::hle::SdrRemote* sdr_remote = nullptr;
    ps2r::triage::InputLog* pad_replay = nullptr;
    std::string* pad_input_source = nullptr;
    std::unique_ptr<PlatformInput>* pad_probe = nullptr;   // harness.live_pad_probe (null = off)
    PadStampDuty* pad_stamp = nullptr;
    std::unique_ptr<PlatformInput>* live_input = nullptr;
    // A5 qol.skip_movies: ArmPlayMode's poll of the live pad for the movie-skip
    // press; the FMV park's display tick calls it (null outside play mode).
    std::function<void()>* movie_skip_poll = nullptr;
    // A5 qol.fast_forward: the cutscene fast-forward decision (R2 held while the
    // title's video controller plays); its extra fields feed the wall-locked
    // boundary (BoundarySequencer::SetExtraFields). Play mode + live pad only.
    title::CutsceneFastForward* fast_forward = nullptr;
    const uint32_t* input_save_selection = nullptr;
    ps2r::app::InputSave* input_save = nullptr;
    ps2r::platform::sdl::AudioPullRing* audio_ring = nullptr;
    ps2r::app::ClockCensus* clock_census = nullptr;
    ps2r::platform::sdl::SdlAudioSink* audio_sink = nullptr;
    std::vector<float>* audio_gain_buf = nullptr;
    const float* audio_gain = nullptr;
    std::unique_ptr<ps2r::app::SessionRuntime>* session = nullptr;
    // The Display page's rows bound to the video stack (play mode only).
    std::unique_ptr<ps2r::app::DisplayRows>* display_rows = nullptr;
    // video.aspect: the one display shape of the leg, shared by
    // the session presenter (window size in, picture ratio out) and the title's
    // aspect model installed with the Graphics application.
    std::shared_ptr<ps2r::gs::AspectSelection> aspect;
    const std::vector<ps2r::verify::MemorySpan>* verify_spans = nullptr;
    int64_t* audio_first_nonsilent_window = nullptr;
    ps2r::app::FrameCensus* frame_census = nullptr;
    FootprintCensus* footprint_census = nullptr;
#if PS2R_IOP_ORACLE
    EeRpcServer* ee_rpc_server = nullptr;
#endif
    BoundaryProtocol* protocol = nullptr;
    uint64_t* display_ticks = nullptr;
    uint64_t* display_ticks_late = nullptr;
    uint64_t* audio_device_quanta = nullptr;
    uint64_t* audio_device_nonsilent = nullptr;
    std::function<void(const float*)>* push_audio_ring = nullptr;
    const bool* audio_field_pump_on = nullptr;
    uint64_t* snd_pumps_fields = nullptr;
    uint64_t* audio_pumped_tick = nullptr;
    std::function<void(uint64_t)>* audio_field_pump = nullptr;
    std::function<void(bool)>* display_tick = nullptr;
    uint64_t* wait_display_ticks = nullptr;
    uint64_t* wait_hook_calls = nullptr;
    uint64_t* cdvd_park_waits = nullptr;
    std::string* first_syscalls = nullptr;
    uint64_t* syscall_count = nullptr;
    LibdmaBindings* libdma = nullptr;
    ps2r::ipu::GuestMemoryAdapter* guest_mem = nullptr;
    title::MovieStream* movie_stream = nullptr;
    std::function<int32_t(uint8_t*, uint32_t)>* read_movie_sectors = nullptr;
    const ps2r::ipu::FmvContextLayout* fmv_layout = nullptr;
    ps2r::ipu::FmvSectorSourceAdapter* fmv_sectors = nullptr;
    ps2r::sound::FmvPcmInput* fmv_pcm = nullptr;
    ps2r::app::MovieSound* movie_sound = nullptr;
    ps2r::ipu::MpegApi* mpeg = nullptr;
    ps2r::ipu::LibmpegBindings::Hooks* mpeg_hooks = nullptr;
    ps2r::ipu::LibmpegBindings* libmpeg = nullptr;
    ps2r::ipu::LibipuBindings* libipu_bind = nullptr;   // L7: the oracle binary's; null in the play binary (no IPU model linked: the rows trap)
    title::GameLibraries* game_libraries = nullptr;
    ps2r::app::GraphicsApplication* graphics_application = nullptr;
    ps2r::app::FileServices* file_services = nullptr;
    ps2r::app::MovieStorage* movie_storage = nullptr;
    const int* meta_arena_hooks = nullptr;
    FmvAbortHold* fmv_abort_hold = nullptr;
    ChunkStateCensus* chunk_state_census = nullptr;
    title::MenuExtension* menu_extension = nullptr;   // play mode + ui.menu_pages; null otherwise
    title::RetailFixes* retail_fixes = nullptr;       // A5 fixes.* (owned bodies; nothing installed when all off)
    PadReadinessHook* pad_readiness = nullptr;
    const bool* pad_hook_installed = nullptr;
    LoaderSpinLift* loader_spin_lift = nullptr;
    const bool* headroom_query_recompiled = nullptr;
    ps2r::app::ServiceTraceLog* service_trace = nullptr;
    ps2r::app::StartupOwnership* startup_ownership = nullptr;
    StateRestorer* restorer = nullptr;
    ps2r::app::SessionClock* session_clock = nullptr;
    HeapSection* heap_section = nullptr;
    VuMemorySection* vu_memory_section = nullptr;
    ThreadSection* thread_section = nullptr;
    uint64_t* dmac_save_unquiesced = nullptr;
    VifsSection* vifs_section = nullptr;
    ps2r::gs::GsSections* gs_sections = nullptr;
    GifsSection* gifs_section = nullptr;
    uint64_t* gifs_restore_tail_bytes = nullptr;
    PadsSection* pads_section = nullptr;
    Vu1RegisterSection* vu1_register_section = nullptr;
    ps2r::app::ServiceRestore* service_restore = nullptr;
    ps2r::app::ServiceImport* service_import = nullptr;
    uint32_t* restored_vsync_frame = nullptr;
#if PS2R_IOP_ORACLE
    uint64_t* iopm_restored = nullptr;
    std::function<std::string()>* iop_threads_census = nullptr;
    std::function<std::string()>* iop_semas_census = nullptr;
    std::string* iop_threads_restored = nullptr;
#endif
    ps2r::app::MovieCheckpoint* movie_checkpoint = nullptr;
    uint64_t* fmv_active_frames = nullptr;
    uint64_t* fmv_window_first = nullptr;
    uint64_t* fmv_window_last = nullptr;
    uint64_t* fmv_served_at_prev_boundary = nullptr;
    bool* fmv_window_open = nullptr;
    ps2r::app::SessionCapture* checkpoint = nullptr;
    ps2r::savestate::Quiescence* quiesce = nullptr;
    TickThread* tick_thread = nullptr;
    ps2r::gs::PresentPolicy* present_policy = nullptr;
    std::vector<uint8_t>* held_rgba = nullptr;
    uint64_t* held_hash = nullptr;    // the last live frame's lane hash (the Hold line)
    int* held_w = nullptr;
    int* held_h = nullptr;
    LegCloser* closer = nullptr;
    std::function<void()>* census_services = nullptr;
    std::function<void()>* write_census = nullptr;
    ps2r::app::SessionWorkers* session_workers = nullptr;
    ps2r::app::SavestateTrigger* save_trigger = nullptr;
    ps2r::app::DisplaySequence* display_sequence = nullptr;   // harness.display_sequence (null = none)
};
