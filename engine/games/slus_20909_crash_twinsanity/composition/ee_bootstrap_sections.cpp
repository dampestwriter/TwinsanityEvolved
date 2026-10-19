// Phase functions of the composition root (EeBootstrap::Run split, ARCHITECTURE section 11):
// each phase unpacks the upstream objects it uses from LegState, constructs its own, registers
// the ones later phases need and tail-calls the next phase, so object lifetimes nest exactly as
// the former single scope did (later phases' objects are destroyed first).
#include "games/slus_20909_crash_twinsanity/composition/leg_state.h"

int EeBootstrap::RegisterSections(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    const TitleProfile& title = *s.title;
    const EeBootstrap::Paths& paths = *s.paths;
    const bool& play_mode = *s.play_mode;
    MainMemory& ram = *s.ram;
    Scratchpad& spad = *s.spad;
    MetaArena& arena = *s.arena;
    Intc& intc = *s.intc;
    EmuDomain& domain = *s.domain;
    EeTimers& timers = *s.timers;
    TickSource& tick_source = *s.tick_source;
    ps2r::gs::GsState& gs_state = *s.gs_state;
    GifRegs& gif_regs = *s.gif_regs;
    GifParser& gif_parser = *s.gif_parser;
    VuUnit& vu0 = *s.vu0;
    VuUnit& vu1 = *s.vu1;
    Vu1HleRuntime& vu1_hle = *s.vu1_hle;
    Vif& vif = *s.vif;
    Dmac& dmac = *s.dmac;
    ps2r::app::GraphicsOutput& graphics_output = *s.graphics_output;
    ps2r::app::EffectsApplication& effects_application = *s.effects_application;
    std::unique_ptr<MtvuStack>& mtvu = *s.mtvu;
#if PS2R_IOP_ORACLE
    IopModuleTable& iop_table = *s.iop_table;
    ps2r::title::stream_irx::StreamIrxPlugin& stream_irx = *s.stream_irx;
#endif
    ReadEngine& read_engine = *s.read_engine;
    ps2r::sound::SpuCore& spu_core = *s.spu_core;
    ps2r::app::SoundOutput& sound_output = *s.sound_output;
    BoundarySequencer& sequencer = *s.sequencer;
    GameBoundary& game_boundary = *s.game_boundary;
    McCommandModel& mc_model = *s.mc_model;
    PadProtocol& pad_protocol = *s.pad_protocol;
    ps2r::sound::hle::SdBlockTrans& sd_block_trans = *s.sd_block_trans;
    ps2r::app::InputSave& input_save = *s.input_save;
    BoundaryProtocol& protocol = *s.protocol;
    title::MovieStream& movie_stream = *s.movie_stream;
    ps2r::app::MovieSound& movie_sound = *s.movie_sound;
    ps2r::ipu::MpegApi& mpeg = *s.mpeg;
    ps2r::ipu::LibmpegBindings::Hooks& mpeg_hooks = *s.mpeg_hooks;
    title::GameLibraries& game_libraries = *s.game_libraries;
    ps2r::app::GraphicsApplication& graphics_application = *s.graphics_application;
    ps2r::app::FileServices& file_services = *s.file_services;
    // Restore memory and game clock before dependent owners. Retained sections
    // keep their original ordering; selected services require explicit state.
    MemorySections memory_sections(ram, spad, arena.Data(), arena.Size());
    StateRestorer restorer(memory_sections);
    ps2r::app::SessionClock session_clock(domain, tick_source, sequencer.Ticks(),
        std::string("SLUS-20909:services-v1:") + cfg.Fingerprint());
    if (!restorer.Registry().Register(session_clock.Section())) return 2;
    // Frozen anchors contain these five records which this root historically
    // did not restore. Keep that bounded compatibility while M3/M4/M12 migrate
    // their ownership. Every new REQUIRED service section must have an active
    // owner; otherwise reject before changing RAM or stopping live threads.
    restorer.RequireSectionOwners({
        ps2r::savestate::TAG_FILE, ps2r::savestate::TAG_CNTR, ps2r::savestate::TAG_MISC,
        ps2r::savestate::TAG_STRM, ps2r::savestate::TAG_SIFS});
    if (!game_libraries.RegisterCheckpointSections(restorer.Registry())) {
        std::fprintf(stderr, "nativeee: game-library checkpoint owner registration failed\n");
        return 2;
    }
    // BUG-085: restore META's allocation bookkeeping after its bytes so the
    // next allocation cannot re-vend a live descriptor.
    HeapSection heap_section(HeapSection::TailCodec{
        [&arena](std::vector<uint8_t>& out) { arena.SerializeState(out); },
        [&arena](const uint8_t* p, size_t len, size_t* consumed, std::string* err) {
            return arena.ValidateState(p, len, consumed, err);
        },
        [&arena](const uint8_t* p, size_t len, size_t* consumed) { return arena.RestoreState(p, len, consumed); }});
    heap_section.Register(restorer.Registry());
    // The VU memories right after the memory sections: a restored micro memory
    // invalidates the unit's decoded image and the HLE runtime's generation.
    VuMemorySection vu_memory_section(vu0, vu1);
    Vu1HleRuntime* worker_vu1_hle = s.worker_vu1_hle;
    // With the MTVU stack the worker's unit is the VU1 the title drives (VIF1 is
    // bound to it): a restored VU1 memory / register file is mirrored into it
    // (MtvuStack::MirrorVu1) so the restored title's first kick runs the
    // restored program, in the live F8 and in --load-state alike.
    vu_memory_section.SetOnVu1Restored([&vu1_hle, worker_vu1_hle, &mtvu, &vu1] {
        if (mtvu) mtvu->MirrorVu1(vu1);
        vu1_hle.BumpExternalGeneration();
        if (worker_vu1_hle) worker_vu1_hle->BumpExternalGeneration();
    });
    vu_memory_section.Register(restorer.Registry());
    ThreadSection thread_section(ThreadManager::Instance());
    KernelSections kernel_sections(Semaphores::Instance(), EventFlags::Instance());
    IntcSection intc_section(intc, SyscallTable::Instance());
    TimerSection timer_section(timers, title.quirks.boot_t0_mode);
    thread_section.Register(restorer.Registry());
    kernel_sections.Register(restorer.Registry());
    intc_section.Register(restorer.Registry());
    // DMAC after INTC: the channel / global registers and the handler records.
    // Capture quiesces first: the deferred VIF1 kick is flushed so the file
    // never carries a parked kick (the restore clears the parked kick and the
    // tails anyway); a VIF1 channel still running at capture is counted in
    // run.meta (dmac_save_unquiesced) -- the frame boundary is the save point
    // and item 5.5's tail chase is what settles it.
    uint64_t dmac_save_unquiesced = 0;
    {
        ps2r::savestate::SectionSpec dmac_spec = dmac.SavestateSection();
        dmac_spec.serialize = [&dmac, &dmac_save_unquiesced](std::vector<uint8_t>& out) {
            dmac.FlushDeferredVif1();
            if (dmac.HasDeferredVif1() || (dmac.channel(static_cast<int>(DmacChannelId::Vif1)).chcr & DmacChcr::kStr)) ++dmac_save_unquiesced;
            dmac.SerializeSection(out);
        };
        restorer.Registry().Register(dmac_spec);
    }
    timer_section.Register(restorer.Registry());
    // VIFS then GIFS after TIMR (GSPR / GSST sit between them in the file).
    VifsSection vifs_section(vif);
    vifs_section.Register(restorer.Registry());
    // Retained scanout/state owners remain until graphics cutover acceptance.
    ps2r::gs::GsSections gs_sections(gs_state);
    // The pipelined queue's split image: the VRAM section reads the pipeline's
    // image (the committed GPU deposits live there), drained first.
    if (s.gs_queue && s.pipeline_vram && *s.pipeline_vram) {
        auto& gs_queue = *s.gs_queue;
        auto& pipeline_vram = *s.pipeline_vram;
        gs_sections.SetVramSource([&gs_queue, &pipeline_vram]() -> const ps2r::gs::VramImage* {
            if (gs_queue) gs_queue->Sync();
            return pipeline_vram.get();
        });
    }
    gs_sections.Register(restorer.Registry());
    GifsSection gifs_section(gif_regs);
    uint64_t gifs_restore_tail_bytes = 0;
    gifs_section.SetOnRestored([&gif_parser, &gifs_restore_tail_bytes] {
        // The parser's buffered tail is empty at a boundary capture; a non-empty
        // tail at restore is counted, not dropped (dropping it would be a GS-side
        // observable; the parser exposes no tail clear that is free of one).
        gifs_restore_tail_bytes += gif_parser.BufferedBytes();
    });
    gifs_section.Register(restorer.Registry());
    // Deselected Input preserves the historical restored pad-area sequence.
    PadsSection pads_section(
        pad_protocol, [](uint32_t a) { return memory::read<uint32_t>(a); }, PadsSection::SeqRestore::FromImage);
    if (!input_save.InputSelected()) pads_section.Register(restorer.Registry());
    // VU1R (optional) after SIFS and before SDRX: the VU1 register file.
    Vu1RegisterSection vu1_register_section(vu1);
    vu1_register_section.SetOnRestored([&mtvu, &vu1] { if (mtvu) mtvu->MirrorVu1(vu1); });
    vu1_register_section.Register(restorer.Registry());
    // SDRX and SNDC restore the retained voices and block transfers.
    ps2r::sound::SoundStateCodec sound_state_codec(spu_core, sd_block_trans);
    sound_state_codec.Register(restorer.Registry());
    ps2r::app::SessionServices service_checkpoint(
        cfg.hle.stream_files || cfg.hle.sound_samples || cfg.hle.music,
        sound_output.StateOwner(), sound_output.OutputOwner());
    // The explicit legacy import (ServiceImport): a frozen anchor of the
    // title's inventory restores under the selected file composition with
    // SVCS/FILS derived from its own RAM0/THRD (never fabricated) and M0's
    // clockless boot clock; it prepares in the preflight and publishes at the
    // two owners' absent positions. Every capture still emits both sections.
    ps2r::app::ServiceImport service_import(service_checkpoint, file_services,
        cfg.hle.sound_samples || cfg.hle.music,
        [&read_engine](uint32_t lba, uint64_t position, uint32_t size, uint8_t* out) {
            const uint32_t skew = static_cast<uint32_t>(position % IsoFs::kSectorSize);
            const uint32_t sectors = (skew + size + IsoFs::kSectorSize - 1) / IsoFs::kSectorSize;
            std::vector<uint8_t> raw(static_cast<size_t>(sectors) * IsoFs::kSectorSize);
            if (read_engine.ReadSectors(lba + static_cast<uint32_t>(position / IsoFs::kSectorSize), sectors, raw.data()) != sectors)
                return false;
            std::copy_n(raw.begin() + skew, size, out);
            return true;
        },
        [&domain] { return domain.DomainTick(); });
    service_import.Approve(paths.title_dir + "/" + title.save.expected_hashes, cfg.harness.load_state);
    const auto publish = [&service_import](bool (ps2r::app::ServiceImport::*publish)(std::string*)) {
        std::string error;
        if (!(service_import.*publish)(&error)) throw std::runtime_error(error);
    };
    if (service_checkpoint.Enabled() && !restorer.Registry().Register(service_checkpoint.Section(
            [publish] { publish(&ps2r::app::ServiceImport::PublishFiles); }))) return 2;
    if (file_services.Enabled() && !restorer.Registry().Register(file_services.Section(
            [publish] { publish(&ps2r::app::ServiceImport::PublishDisc); }))) return 2;
    ps2r::app::ServiceRestore service_restore(session_clock, service_checkpoint, file_services);
    service_restore.SetImport(&service_import);
    if (!service_restore.RegisterValidation(restorer.Registry())) return 2;
    restorer.SetImageValidation([&](const auto& meta, const auto& blobs, std::string* error) {
        return service_restore.ValidateImage(meta, blobs, true, error);
    });
    if (service_checkpoint.Enabled() || file_services.Enabled())
        restorer.AddImagePreflight([&](const auto& blobs, std::string* error) {
            std::string why;
            return !service_import.Admits(blobs, &why) || service_import.Prepare(blobs, error);
        }, [&] { service_import.Discard(); });
    // MCHL (optional) after SNDC, the reference file's order: the libmc pending
    // completion pair + the per-(port, slot) card-seen flags.
    MchlSection mchl_section(mc_model);
    if (!input_save.SavesSelected()) mchl_section.Register(restorer.Registry());
    if (!input_save.Register(restorer.Registry(), &session_clock)) return 2;
    // IOPM only supports the historical boot import, not live F8 rewind.
    uint32_t restored_vsync_frame = 0;
#if PS2R_IOP_ORACLE
    uint64_t iopm_restored = 0;
    // The LIVE IOP kernel's thread table as a census string `tid:State/Wait,...`
    // (registered tids only) and the active semaphores' token counts `sid:count`.
    auto iop_threads_census = [&iop_table]() {
        static const char* const kState[] = {"Dormant", "Ready", "Run", "Wait"};
        static const char* const kWait[] = {"None", "Sleep", "Delay", "Sema", "EventFlag", "Mbx", "Rpc"};
        const ps2r::iop::IopKernel& k = iop_table.Kernel();
        std::string out;
        for (int t = 1; t < ps2r::iop::IopKernel::kMaxThreads; t++) {
            if (!k.ThreadRegistered(t)) continue;
            if (!out.empty()) out += ',';
            out += std::to_string(t) + ":" + kState[static_cast<int>(k.ThreadState(t))] + "/" +
                   kWait[static_cast<int>(k.ThreadWaitKind(t))];
        }
        return out.empty() ? std::string("none") : out;
    };
    auto iop_semas_census = [&iop_table]() {
        const ps2r::iop::IopKernel& k = iop_table.Kernel();
        std::string out;
        for (int sid = 0; sid < ps2r::iop::IopKernel::kMaxSemas; sid++) {
            if (!k.SemaActive(sid)) continue;
            if (!out.empty()) out += ',';
            out += std::to_string(sid) + ":" + std::to_string(k.SemaCount(sid));
        }
        return out.empty() ? std::string("none") : out;
    };
    std::string iop_threads_restored = "none";
    {
        ps2r::iop::IopmSection::Deps iopm_deps =
            stream_irx.IopmDeps([&restored_vsync_frame] { return static_cast<uint64_t>(restored_vsync_frame); });
        const std::function<void()> plugin_after_restore = iopm_deps.after_restore;
        iopm_deps.after_restore = [plugin_after_restore, &iopm_restored] {
            if (plugin_after_restore) plugin_after_restore();
            ++iopm_restored;
        };
        restorer.Registry().Register(ps2r::iop::IopmSection::MakeSpec(iopm_deps));
    }
#endif
    if (!graphics_application.RegisterState(restorer)) return 2;
    if (effects_application.Selected()) {
        // Reuse the existing GFRM/GSCN owners; register the shared source state only when neither did.
        const bool frame = restorer.Registry().Has(title::FrameState::kTag);
        const bool scene = restorer.Registry().Has(title::SceneState::kTag);
        if (frame != scene || (!frame && !graphics_output.RegisterSourceState(restorer))) return 2;
    }
    if (!effects_application.Register(restorer.Registry(), [&](auto preflight) {
            restorer.AddImagePreflight(std::move(preflight));
        })) return 2;
    ps2r::app::MovieCheckpoint movie_checkpoint(game_libraries.Movie(), movie_stream, mpeg,
        mpeg_hooks.now_tick, movie_sound.StateOwner());
    if (!movie_checkpoint.RegisterBoot(restorer.Registry(), paths.title_dir + "/" + title.save.expected_hashes,
                                      cfg.harness.load_state) ||
        !movie_checkpoint.RegisterPreparation(restorer)) return 2;
    restorer.SetPreRestore([&](std::string* reason) {
        if (!effects_application.CanCheckpoint(reason)) return false;
        if (!movie_checkpoint.CanCheckpoint(reason) || !service_restore.CanCheckpoint(reason) ||
            !graphics_application.CanRestore(reason)) return false;
        // CanRestore settled the output. A fresh --load-state process has a
        // CLOSED recording (nothing drawn yet): the strict reset. A LIVE restore
        // (F8 in the running session) stands at an admitted in-play boundary
        // whose recording is OPEN -- its owned batches Pending, the GXPD digest
        // of the settle -- so the live form cancels them first (the restored
        // title draws its first field without those groups, attributed as the
        // process restore does: INTERFACES section 13).
        if (graphics_output.UnlinkedAtBoundary().items > 0) graphics_output.ResetForRestore();
        else graphics_output.ResetAfterQuiescence();
        domain.ReAnchor(tick_source.RawCount());
        return true;
    });
    // Active movies defer savestates and inform the present policy and census.
    uint64_t fmv_active_frames = 0;
    // The served-picture window tells diff_legs which frames to pair by picture.
    uint64_t fmv_window_first = 0, fmv_window_last = 0, fmv_served_at_prev_boundary = 0;
    bool fmv_window_open = false;
    ps2r::app::SessionCapture checkpoint(restorer.Registry(), game_boundary, cfg,
        game_libraries, movie_stream, mpeg, mtvu.get(), graphics_output, service_restore);
    auto& quiesce = checkpoint.Conditions();
    movie_checkpoint.RegisterConditions(quiesce);
    graphics_application.RegisterConditions(quiesce);
    effects_application.RegisterConditions([&](const char* name, auto condition) { quiesce.Register(name, std::move(condition)); });
    quiesce.Register("game_save", [&](std::string* error) { return input_save.CanCheckpoint(error); });
    // Play's field ticker joins before its protocol/sequence can be destroyed.
    TickThread tick_thread(tick_source, sequencer.Ticks(), protocol);
    if (play_mode) tick_thread.Start();
    // The wall-locked Play boundary: with a pacer running, the
    // boundary's TickAdvance slot parks for the pacer's next noted tick and fires
    // the noted ticks instead of its own Boundary tick -- one T0 field per wall
    // vblank, the reference's paced vblank wait -- and a tick a guest waiter
    // fires rides 1:1 (no quantum absorption: a pace park of N ticks costs N
    // fields). Bound ONLY in play mode: the deterministic policy and the headless
    // Freerun mode never reach the branch (pin: RootPlayPolicyInert).
    if (play_mode) sequencer.BindPacer(&protocol);
    // Row 6.7: the present policy -- the play-mode host shows a picture at every
    // boundary (the display rate under the wall lock): the live grab when the GS
    // produced one, else the last shown image again (a held frame buffer, as the
    // CRTC re-reads it), nothing only before the first image; FMV/load phases
    // take the same table (the FmvActive signal's third consumer: census).
    // video.fps_cap is bound here (0 = every field). Play mode only: the Verify
    // slot's deterministic present rule below is untouched.
    ps2r::gs::PresentPolicy present_policy;
    present_policy.SetFpsCap(static_cast<uint32_t>(cfg.video.fps_cap < 0 ? 0 : cfg.video.fps_cap));
    std::vector<uint8_t> held_rgba;   // the last shown image (the Hold source; empty when the hash suffices)
    uint64_t held_hash = 0;           // its frames-lane hash
    int held_w = 0, held_h = 0;

    s.restorer = &restorer;
    s.session_clock = &session_clock;
    s.heap_section = &heap_section;
    s.vu_memory_section = &vu_memory_section;
    s.thread_section = &thread_section;
    s.dmac_save_unquiesced = &dmac_save_unquiesced;
    s.vifs_section = &vifs_section;
    s.gs_sections = &gs_sections;
    s.gifs_section = &gifs_section;
    s.gifs_restore_tail_bytes = &gifs_restore_tail_bytes;
    s.pads_section = &pads_section;
    s.vu1_register_section = &vu1_register_section;
    s.service_restore = &service_restore;
    s.service_import = &service_import;
    s.restored_vsync_frame = &restored_vsync_frame;
#if PS2R_IOP_ORACLE
    s.iopm_restored = &iopm_restored;
    std::function<std::string()> iop_threads_census_fn = iop_threads_census;
    s.iop_threads_census = &iop_threads_census_fn;
    std::function<std::string()> iop_semas_census_fn = iop_semas_census;
    s.iop_semas_census = &iop_semas_census_fn;
    s.iop_threads_restored = &iop_threads_restored;
#endif
    s.movie_checkpoint = &movie_checkpoint;
    s.fmv_active_frames = &fmv_active_frames;
    s.fmv_window_first = &fmv_window_first;
    s.fmv_window_last = &fmv_window_last;
    s.fmv_served_at_prev_boundary = &fmv_served_at_prev_boundary;
    s.fmv_window_open = &fmv_window_open;
    s.checkpoint = &checkpoint;
    s.quiesce = &quiesce;
    s.tick_thread = &tick_thread;
    s.present_policy = &present_policy;
    s.held_rgba = &held_rgba;
    s.held_hash = &held_hash;
    s.held_w = &held_w;
    s.held_h = &held_h;
    return CensusServices(s);
}
