// Phase functions of the composition root (EeBootstrap::Run split, ARCHITECTURE section 11):
// each phase unpacks the upstream objects it uses from LegState, constructs its own, registers
// the ones later phases need and tail-calls the next phase, so object lifetimes nest exactly as
// the former single scope did (later phases' objects are destroyed first).
#include "games/slus_20909_crash_twinsanity/composition/leg_state.h"

int EeBootstrap::ArmIopAndMedia(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    const TitleProfile& title = *s.title;
    MainMemory& ram = *s.ram;
    AddressMap& map = *s.map;
    MmioRouter& mmio = *s.mmio;
    Intc& intc = *s.intc;
    EmuDomain& domain = *s.domain;
    EeTimers& timers = *s.timers;
    TickSource& tick_source = *s.tick_source;
    FrameRendezvous& rendezvous = *s.rendezvous;
    std::atomic<bool>& running = *s.running;
    ps2r::gs::GsState& gs_state = *s.gs_state;
    GifRegs& gif_regs = *s.gif_regs;
    VuUnit& vu0 = *s.vu0;
    VuUnit& vu1 = *s.vu1;
    Vif& vif = *s.vif;
    Dmac& dmac = *s.dmac;
    std::unique_ptr<MtvuStack>& mtvu = *s.mtvu;
    // MMIO ranges (last-registered wins: each displaces its MmioSink row) and
    // the VU memory windows (pointer memory on the AddressMap, not MMIO).
    mmio.Register(0x10008000u, 0x6064u, dmac, "DMAC");
    mmio.Register(0x1000F520u, 0x10u, dmac, "D_ENABLER");
    mmio.Register(0x1000F590u, 0x10u, dmac, "D_ENABLEW");
    mmio.Register(GifRegs::kBase, GifRegs::kSize, gif_regs, "GIF");
    // The GS privileged page: one range over the façade (64-bit accesses to
    // WritePrivileged / ReadPrivileged, 32-bit halves merged by GsPrivRegs);
    // displaces the sink's GS_CSR / GS_PRIV idle rows.
    ps2r::gs::GsMmioHandler gs_mmio(gs_state);
    mmio.Register(MmioRouter::kGsPrivBase, MmioRouter::kGsPrivEnd - MmioRouter::kGsPrivBase, gs_mmio, "GS_PRIV");
#if PS2R_IOP_ORACLE
    // IPU DMA: ch3 drains output to RAM; ch4 feeds RAM into input.
    // Retry armed output when pixels arrive. CPU FIFO ports remain unmapped.
    // The IPU command/DMA model (ledger L7) is the ORACLE binary's: in the play
    // binary the owned movie decoder serves every FMV (movie_decoder pictures>0,
    // ipu_commands/ipu_operations/libipu_calls all 0 on the s3 route), so the IPU
    // window stays the router's idle sink and DMAC ch3/ch4 have no device bound.
    ps2r::ipu::IpuDevice ipu;
    mmio.Register(0x10002000u, ps2r::ipu::IpuDevice::kWindowBytes, ipu, "IPU");
    dmac.SetIpuFeedInput([&ipu](uint8_t* host, size_t bytes) { ipu.FeedInputFifo(host, bytes); });
    dmac.SetIpuDrainOutput([&ipu](uint8_t* host, size_t bytes) { return ipu.DrainOutputFifo(host, bytes); });
    ipu.SetOutputReady([&dmac] {
        if (dmac.channel(3).chcr & DmacChcr::kStr) dmac.StartChannel(3);
    });
    ps2r::ipu::DmacChannelFaceAdapter ipu_dmac_face(dmac.channel(static_cast<int>(DmacChannelId::IpuFrom)),
        dmac.channel(static_cast<int>(DmacChannelId::IpuTo)), [&dmac](uint32_t ch) { dmac.StartChannel(static_cast<int>(ch)); });
    ps2r::ipu::LibipuApi libipu(ipu, ipu_dmac_face);
#endif
    // The VIF ranges go through the gate: the reference routed only FBRST/ERR
    // reads and FBRST/ERR + VIF1-FIFO writes to its unit (every other VIF
    // register read 0 / was dropped) and the baselines were recorded under
    // that surface, so the router presents it, not the full register file.
    // Under MTVU the VIF1 windows sync with the worker first (a VIF1_STAT read
    // sees the busy view while segments are in flight); the gate is the inner handler.
    VifMmioGate vif_gate(vif);
    MmioHandler& vif1_window = mtvu ? mtvu->Vif1Window(vif_gate) : static_cast<MmioHandler&>(vif_gate);
    mmio.Register(Vif::kVif0Base, Vif::kRegsSize, vif_gate, "VIF0");
    mmio.Register(Vif::kVif1Base, Vif::kRegsSize, vif1_window, "VIF1");
    mmio.Register(Vif::kVif1FifoBase, Vif::kFifoSize, vif1_window, "VIF1_FIFO");
    map.RegisterHwWindow(0x11000000u, 0x1000u, vu0.state().micro, "VU0_MICRO");
    map.RegisterHwWindow(0x11004000u, 0x1000u, vu0.state().data, "VU0_DATA");
    map.RegisterHwWindow(0x11008000u, 0x4000u, vu1.state().micro, "VU1_MICRO");
    map.RegisterHwWindow(0x1100C000u, 0x4000u, vu1.state().data, "VU1_DATA");
    // The DMAC's callbacks. INT1 (the DMAC's CPU interrupt line) has no Intc
    // cause: it is counted (the mask is 0 on every cold boot, so the slot never
    // fires there). The SIF1 command-header HLE does not exist yet: counted and
    // warned once. IPU FROM/TO are the IpuDevice's (bound above).
    uint64_t dmac_int1_raises = 0;
    uint64_t dmac_sif_kicks = 0;
    // The DMAC's observation points at the sce surface (item 5.5): the frame
    // sync's tail chase and sceGsSyncPath's flush, counted with how many found
    // a parked VIF1 kick (the libdma sync rows count on LibdmaBindings).
    uint64_t dmac_tail_chases = 0, dmac_tail_chases_parked = 0;
    uint64_t gs_syncpath_flushes = 0, gs_syncpath_parked = 0;
    dmac.SetInterruptRaise([&dmac_int1_raises] { ++dmac_int1_raises; });
#if PS2R_IOP_ORACLE
    // The SIF DMA model's DMAC face (ledger L5): SIF0/SIF1 channel kicks are counted
    // and warned once in the ORACLE binary (the IOP arm's SIF command-header path, no
    // HLE bound). The play binary binds nothing: its SIF traffic is the sceSif rows
    // over the owned StreamTransport (sif_rpc_calls), never a DMAC channel 5/6 kick
    // (dmac_sif_kicks=0 on every play census; the row keeps that value).
    dmac.SetSifCallback([&dmac_sif_kicks](int ch, const DmacChannelRegs& regs) {
        if (dmac_sif_kicks++ == 0)
            PS2R_LOG(HOST, Warn, "DMAC SIF%d kick (madr=0x%08x qwc=%u) with no SIF command-header HLE bound", ch - 5,
                     regs.madr, regs.qwc);
    });
#endif
#if PS2R_IOP_ORACLE
    // IOP (the ORACLE binary only): the module table owns the
    // cooperative IOP kernel; its FRAME pump points bind to boundary slots 5/6
    // below. STREAM.IRX is booted from here, before the EE runs (the original's
    // order: the module load preceded the CPU thread), with the EE-RAM sink of
    // IOP->EE SIF DMA bound to main RAM.
    static_assert(ps2r::title::stream_irx::ModuleLayout::kRpcServerId == ps2r::app::kStreamRpcServerId);
    IopModuleTable iop_table(cfg);
    ps2r::iop::IopMemory iop_mem;
    MainMemoryEeRamSink ee_ram_sink(ram);
#else
    (void)ram;
#endif
    // Shared read-only media: Files reads bytes directly; ReadEngine supplies
    // remaining sector/module callers. Missing media stays an explicit error.
    IsoFs iso_fs;
    std::string iso_error;
    const std::string iso_path = !cfg.paths.iso.empty() ? cfg.paths.iso : cfg.paths.game_files + "/" + title.disc_image_file;
    const bool iso_open = !title.disc_image_file.empty() && iso_fs.Open(iso_path, &iso_error);
    if (!iso_open) std::fprintf(stderr, "nativeee: disc image: %s\n", iso_error.c_str());
    ReadEngine read_engine(iso_fs);
    StreamReader stream_reader(iso_open ? &read_engine : nullptr);   // the sceCdSt* ring
    // Mixer pumps either owned output or the comparison core at the logical
    // boundary. The playback device never advances game time.
    ps2r::sound::SpuCore spu_core;
    ps2r::app::SoundOutput sound_output(cfg.hle.sound_samples, cfg.hle.music, cfg.hle.audio_mix);
    ps2r::sound::Mixer mixer(spu_core);
    sound_output.BindMixer(mixer);
#if PS2R_IOP_ORACLE
    ps2r::title::stream_irx::StreamIrxPlugin stream_irx(iop_table, iop_mem, spu_core, iso_open ? &read_engine : nullptr, &ee_ram_sink);
    // iop.restored_timer_due: the first due of a RUNNING hard timer the IOPM
    // restore re-arms (one period after the restored now, or at the restore);
    // bound before any restore can run.
    stream_irx.Timrman().SetRestoredTimerDue(cfg.iop.restored_timer_due);
#endif
    // Service-only startup (hle.startup=native): the IOP never boots. The
    // module table stays empty, no kernel thread or hard timer ever exists, and
    // the title's STREAM/libsd/heap/LoadModule surface is answered by the native
    // owners (NativeStartup). `retained` (the deterministic profile's oracle arm)
    // boots STREAM.IRX exactly as before.
    ps2r::app::NativeStartup::Owners native_owners;
    native_owners.input = cfg.hle.input_service;
    native_owners.saves = cfg.hle.save_storage;
    native_owners.audio = cfg.hle.sound_samples && cfg.hle.audio_mix;
    native_owners.banks = cfg.hle.sound_bank_headers;
    native_owners.transport = cfg.hle.stream_files && cfg.hle.sound_samples && cfg.hle.music && cfg.hle.music_buffers;
    native_owners.files = cfg.hle.files;
    native_owners.movie_cursor = cfg.hle.movie_start;
    ps2r::app::NativeStartup native_startup(
        cfg.hle.startup == ps2r::StartupMode::Native, native_owners,
        [](uint32_t a) { return memory::read<uint8_t>(a); }, [](uint32_t a, uint8_t v) { memory::write<uint8_t>(a, v); });
#if PS2R_IOP_ORACLE
    ps2r::title::stream_irx::StreamIrxPlugin::BootReport irx_boot;
    if (native_startup.Selected()) {
        irx_boot.error = "native startup: the IOP never boots";
        PS2R_LOG(HOST, Info, "STREAM.IRX: not booted (hle.startup=native; StreamTransport serves 0x12345)");
    } else {
        const std::string irx_path = cfg.paths.game_files + "/" + title.stream_irx_file;
        std::ifstream in(irx_path, std::ios::binary);
        std::vector<uint8_t> irx((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (irx.empty()) {
            irx_boot.error = "missing " + irx_path;
        } else {
            irx_boot = stream_irx.Boot(irx.data(), irx.size());
        }
        PS2R_LOG(HOST, Info, "STREAM.IRX: %zu bytes, entry_ran=%d rpc_tid=%d %s", irx.size(),
                 irx_boot.entry_ran ? 1 : 0, irx_boot.rpc_tid, irx_boot.error.c_str());
    }
    const bool iop_booted = irx_boot.entry_ran;
    // sceSifSetDma (syscall 119) moves its bytes EE -> IOP RAM. The copy is clamped to
    // the IOP RAM extent past the (masked) destination: a bogus guest size must not turn
    // into a 4 GB loop on the boundary; the clamped remainder is counted for the census.
    uint64_t sif_dma_clamped = 0;
    SyscallTable::Instance().SetSifDmaCopy([&iop_mem, &sif_dma_clamped](uint32_t ee_src, uint32_t iop_dst, uint32_t size) {
        const uint32_t dst = iop_dst & ps2r::iop::IopMemory::kMask;
        const uint32_t room = ps2r::iop::IopMemory::kSize - dst;
        if (size > room) { sif_dma_clamped += size - room; size = room; }
        for (uint32_t i = 0; i < size; ++i) iop_mem.Write<uint8_t>(dst + i, memory::read<uint8_t>(ee_src + i));
    });
#else
    // The PLAY binary: no IOP is linked. hle.startup=native is the only arm
    // (ConfigLoader refused every other composition before this point), so the
    // title's STREAM/libsd/heap/LoadModule surface is the native owners'.
    const bool iop_booted = false;
    if (!native_startup.Selected()) {
        std::fprintf(stderr, "nativeee: hle.startup=retained reached the composition root of the service-only binary\n");
        return 2;
    }
    PS2R_LOG(HOST, Info, "STREAM.IRX: not linked (service-only binary; StreamTransport serves 0x12345)");
    (void)title;
#endif
    BoundarySequencer sequencer;
    GameBoundary game_boundary(ThreadManager::Instance(), sequencer);
    if (!game_boundary.Attached()) return 2;
    BoundarySequencer::Phase3Deps deps;
    deps.source = &tick_source;
    deps.domain = &domain;
    deps.rendezvous = &rendezvous;
    deps.intc = &intc;
    deps.alarms = &Alarms::Instance();
    deps.timers = &timers;
    deps.queue = &EeEventQueue::Instance();
    deps.running = &running;
    sequencer.InstallPhase3Steps(deps);
    s.gs_mmio = &gs_mmio;
#if PS2R_IOP_ORACLE
    s.ipu = &ipu;
    s.ipu_dmac_face = &ipu_dmac_face;
    s.libipu = &libipu;
#endif
    s.vif_gate = &vif_gate;
    s.dmac_int1_raises = &dmac_int1_raises;
    s.dmac_sif_kicks = &dmac_sif_kicks;
    s.dmac_tail_chases = &dmac_tail_chases;
    s.dmac_tail_chases_parked = &dmac_tail_chases_parked;
    s.gs_syncpath_flushes = &gs_syncpath_flushes;
    s.gs_syncpath_parked = &gs_syncpath_parked;
#if PS2R_IOP_ORACLE
    s.iop_table = &iop_table;
    s.iop_mem = &iop_mem;
    s.ee_ram_sink = &ee_ram_sink;
    s.stream_irx = &stream_irx;
    s.irx_boot = &irx_boot;
    s.sif_dma_clamped = &sif_dma_clamped;
#endif
    s.iop_booted = &iop_booted;
    s.iso_fs = &iso_fs;
    s.iso_open = &iso_open;
    s.read_engine = &read_engine;
    s.stream_reader = &stream_reader;
    s.spu_core = &spu_core;
    s.sound_output = &sound_output;
    s.mixer = &mixer;
    s.native_startup = &native_startup;
    s.sequencer = &sequencer;
    s.game_boundary = &game_boundary;
    return ArmBoundaryServices(s);
}

int EeBootstrap::ArmBoundaryServices(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    Intc& intc = *s.intc;
    EmuDomain& domain = *s.domain;
    FrameRendezvous& rendezvous = *s.rendezvous;
    std::atomic<bool>& running = *s.running;
    ps2r::gs::GsState& gs_state = *s.gs_state;
    ps2r::app::VideoStack& video = *s.video;
    std::unique_ptr<ps2r::gs::DrawIrThreadQueue>& gs_queue = *s.gs_queue;
    ps2r::gs::CountingDrawIrSink& draw_sink = *s.draw_sink;
    std::unique_ptr<MtvuStack>& mtvu = *s.mtvu;
#if PS2R_IOP_ORACLE
    IopModuleTable& iop_table = *s.iop_table;
    ps2r::iop::IopMemory& iop_mem = *s.iop_mem;
    ps2r::title::stream_irx::StreamIrxPlugin& stream_irx = *s.stream_irx;
    ps2r::title::stream_irx::StreamIrxPlugin::BootReport& irx_boot = *s.irx_boot;
#endif
    ps2r::sound::SpuCore& spu_core = *s.spu_core;
    ps2r::sound::Mixer& mixer = *s.mixer;
    BoundarySequencer& sequencer = *s.sequencer;
    // Slot 0 (PresentRendezvous), the GS side (item 6.3): the Phase-3 ticket
    // (issued and awaited exactly as InstallPhase3Steps does; immediate ack until
    // a presenter thread arms the rendezvous), then the frame's VSync job through
    // the sink chain -- counting (drawir frames=) and, with a live video stack,
    // DrawPipeline::OnFrameEnd: the EE frame mark, the reader-band present flush,
    // the display grab (kept for the Verify slot's frames.fhash line), the display
    // quad and the backend present (skipped until the first DISPFB job set the
    // scanout base). Then CSR.VSINT for the vsync edge; the field parity is the
    // derived clock's and the INTC VBON / TIM2 / VBOF edge is the Phase-3 tick body
    // in slot 1 (raised there once per boundary, never again here). SIGNAL /
    // FINISH requests the register stream counted since the last boundary raise
    // the GS interrupt cause here, before slot 1 dispatches the pending ISRs.
    uint64_t gs_frame_boundaries = 0, gs_irq_raises = 0;
    sequencer.SetStep(BoundarySequencer::Step::PresentRendezvous, [&](CpuContext&) {
        const uint32_t ticket = rendezvous.Issue();
        if (!rendezvous.WaitFor(ticket, running)) return false;
        ps2r::gs::JobRec vs{};
        vs.type = static_cast<uint32_t>(ps2r::gs::JobKind::VSync);
        vs.seq = gs_state.Seq();
        vs.src_frame = static_cast<uint32_t>(gs_frame_boundaries);
        vs.arg1 = 1;   // an EE-pushed game-frame boundary
        vs.vram_epoch = gs_state.Vram().UploadEpoch();
        draw_sink.OnFrame(vs);
        // The frame ticket: the Verify slot reads the consumer's grab (LastFrame),
        // so the boundary waits until the consumer thread consumed this VSync job
        // -- under the deterministic profile. The wall-locked play boundary NEVER
        // waits for the consumer (the clock contract, rule 2: frame_waits = 0;
        // the queue drops the older pending frame instead and the Verify slot
        // reads a published copy of the last grab).
        if (gs_queue && !sequencer.WallLocked()) gs_queue->WaitFrame(gs_queue->FrameSeq());
        // The deferred OS window resize (OnWindowResizedOffMain: the consumer's
        // SetWindow, ConfigureDisplay, the checkpoint's restore) applies here, on
        // the game's host thread, in every gs.queue mode.
        video.ApplyPendingWindowResize();
        ++gs_frame_boundaries;
        if (mtvu) mtvu->NotePresented();   // the runahead's consumer side
        gs_state.Priv().SetVsint();
        for (uint32_t n = gs_state.Priv().TakeInterrupts(); n > 0; --n) {
            intc.Raise(Intc::kGs);
            ++gs_irq_raises;
        }
        return true;
    });
    // Slot 4 (MaintDuties) in the original's duty order: CD completion delivery
    // (DeliveryMeter: reads completed during tick k become visible here at k+1,
    // or at the modelled drive's due tick -- cdvd.latency_model) -> RPC
    // end-callbacks -> pad stamp [4E']; the callbacks run as guest calls on the
    // boundary's context.
    DeliveryMeter delivery_meter(cfg.cdvd);
    // The loader's asynchronous reads (LoaderFileIo, the title's five file-IO
    // bodies) are the meter's producer through LoaderModel; the per-epoch byte
    // budget is unmetered (0) until a Config setting binds it.
    DeliveryBudget delivery_budget(0);
    LoaderModel loader_model(delivery_meter, delivery_budget);
#if PS2R_IOP_ORACLE
    ps2r::app::ServiceTimeline service_timeline(cfg.hle.service_time, domain, &iop_table.Kernel().Clock());
#else
    ps2r::app::ServiceTimeline service_timeline(cfg.hle.service_time, domain);   // no remaining services: the clock sits at frame 0
#endif
    // cdvd.delivery_visibility=poll: every loader poll first lands the reads the
    // modelled drive finished inside the current frame; the tick is
    // the domain's, so the drive's time stays a function of emulated time.
    loader_model.SetNowTick([&domain] { return domain.DomainTick(); });
    // Remaining STREAM reads share the logical read position with owned files.
    // ServiceTimeline maps their observed instant into DomainTick time and,
    // when selected, includes work already settled by an owned blocking wait.
    // The resulting deadline maps back to the remaining caller's frame base.
    // Boundary visibility retains its existing scheduling rule.
#if PS2R_IOP_ORACLE
    if (cfg.cdvd.delivery_visibility == ps2r::CdvdDeliveryVisibility::Poll) {
        stream_irx.Cdvdman().SetCompletionDue([&delivery_meter, &domain, &iop_table, &service_timeline](uint32_t lba, uint32_t sectors, uint64_t now_us) {
            const ps2r::iop::IopClock& clock = iop_table.Kernel().Clock();
            const uint64_t base_us = clock.Frames() * ps2r::iop::IopClock::kFrameUs;
            const double frame_ms = static_cast<double>(domain.DomainTick()) * DeliveryMeter::kTickMs;
            const double end_ms = delivery_meter.ChargeReadAtMs(service_timeline.ReadStartMs(now_us), lba, sectors);
            const double delta_ms = end_ms > frame_ms ? end_ms - frame_ms : 0.0;
            return base_us + static_cast<uint64_t>(delta_ms * 1000.0);
        });
    }
#endif
    // Under poll visibility a harness leg writes the model's trace (enq / dlv
    // src=poll|wait end=<ms> / rel / cns / yld, one line per event, prefixed by
    // the domain tick) to <harness out dir>/loader_trace.log -- the evidence
    // lane for the drive timeline; no guest state is read
    // or written by it, and boundary legs write nothing.
    std::ofstream loader_trace;
    if (cfg.cdvd.delivery_visibility == ps2r::CdvdDeliveryVisibility::Poll && !cfg.harness.out_dir.empty()) {
        loader_trace.open(cfg.harness.out_dir + "/loader_trace.log");
        if (loader_trace.is_open())
            loader_model.SetTraceSink([&loader_trace, &domain](const std::string& line) {
                loader_trace << domain.DomainTick() << ' ' << line << std::endl;   // flushed per line: the leg may end by exit()
            });
    }
    // The loader seam's request lane: every windowed-read request (file, offset, size,
    // dest, sync) as one line of <harness out dir>/loader_requests.log, prefixed by
    // the domain tick -- the input of the ring rule's replay pin. No guest state is
    // read or written by it.
    std::ofstream loader_requests;
    if (!cfg.harness.out_dir.empty()) loader_requests.open(cfg.harness.out_dir + "/loader_requests.log");
    RpcRouter rpc_router;
#if PS2R_IOP_ORACLE
    // The STREAM.IRX face: the EE's sceSifBindRpc(0x12345) routes to it.
    ps2r::title::stream_irx::StreamRpcServer stream_server(
        iop_mem, iop_table.Rpc(), [](uint32_t a) { return memory::read<uint8_t>(a); },
        [](uint32_t a, uint8_t v) { memory::write<uint8_t>(a, v); });
    if (irx_boot.entry_ran)
        rpc_router.RegisterServer(ps2r::app::kStreamRpcServerId, stream_server, "STREAM");
    else
#endif
    if (s.native_startup->Selected())   // service-only startup: the owned transport is the server
        rpc_router.RegisterServer(ps2r::app::kStreamRpcServerId, s.native_startup->Server(), "STREAM_NATIVE");
    // The memory card (Phase 4E): the host store under Config.paths.mc_dir, the
    // mcserv command model (cdvd.mc_latency_polls), and the mcserv RPC face the
    // title's recompiled sceMcInit binds (server 0x80000400) -- its init command
    // raises the model's Bound flag the libmc rows gate on.
    McHostStore mc_store(cfg.paths.mc_dir);
    McCommandModel mc_model(cfg.cdvd.mc_latency_polls);
    McservModule mcserv(mc_model, [](uint32_t a, uint32_t v) { memory::write<uint32_t>(a, v); });
    if (!cfg.hle.save_storage) rpc_router.RegisterServer(McservModule::kServerId, mcserv, "MCSERV");
    // The pad (Phase 4E'): PadProtocol writes the scePad wire image into the
    // title's pad areas once per boundary; the padman RPC face serves the two
    // servers libpad binds (0x80000100 command, 0x80000101 pad data); the stamp
    // duty samples the InputLog replay (harness.inputs; neutral when absent)
    // and feeds the recorder (harness.record_input) with the served sample.
    PadProtocol pad_protocol([](uint32_t a, uint32_t v, int size) {
        if (size == 1) memory::write<uint8_t>(a, static_cast<uint8_t>(v));
        else if (size == 2) memory::write<uint16_t>(a, static_cast<uint16_t>(v));
        else memory::write<uint32_t>(a, v);
    });
    const auto pad_now = [&domain] { return domain.DomainTick(); };
    PadmanModule padman(
        pad_protocol, [](uint32_t a) { return memory::read<uint32_t>(a); },
        [](uint32_t a, uint8_t v) { memory::write<uint8_t>(a, v); },
        [](uint32_t a, uint32_t v) { memory::write<uint32_t>(a, v); }, pad_now);
    if (!cfg.hle.input_service) {
        rpc_router.RegisterServer(PadmanModule::kCommandServerId, padman, "PADMAN");
        rpc_router.RegisterServer(PadmanModule::kDataServerId, padman, "PADMAN_DATA");
    }
    // libsdr (the EE half of the SPU2): SdBlockTrans's virtual block clock is the
    // mixer's frame count (audio.sdr_block_ms of it per block); the SDRDRV face
    // 0x80000701 answers the same command set over SIF when the title binds it.
    ps2r::sound::hle::SdBlockTrans sd_block_trans(cfg.audio.sdr_block_ms, [&mixer] { return mixer.FramesMixed(); });
    ps2r::sound::hle::SdrRemote sdr_remote(spu_core, sd_block_trans, [](uint32_t a) { return memory::read<uint32_t>(a); },
                                           [](uint32_t a, uint32_t v) { memory::write<uint32_t>(a, v); });
    rpc_router.RegisterServer(ps2r::sound::hle::SdrRemote::kServerId, sdr_remote, "SDRDRV");
    ps2r::triage::InputLog pad_replay;
    std::string pad_input_source = "neutral";
    if (!cfg.harness.inputs.empty()) {
        std::string err;
        if (pad_replay.LoadFile(cfg.harness.inputs, &err)) pad_input_source = "replay";
        else PS2R_LOG(HOST, Info, "harness.inputs %s not loaded (%s): neutral pad", cfg.harness.inputs.c_str(), err.c_str());
    }
    PadStampDuty pad_stamp(pad_protocol, &pad_replay, nullptr);
    s.gs_frame_boundaries = &gs_frame_boundaries;
    s.gs_irq_raises = &gs_irq_raises;
    s.delivery_meter = &delivery_meter;
    s.loader_model = &loader_model;
    s.service_timeline = &service_timeline;
    s.loader_requests = &loader_requests;
    s.rpc_router = &rpc_router;
#if PS2R_IOP_ORACLE
    s.stream_server = &stream_server;
#endif
    s.mc_store = &mc_store;
    s.mc_model = &mc_model;
    s.mcserv = &mcserv;
    s.pad_protocol = &pad_protocol;
    std::function<uint64_t()> pad_now_fn = pad_now;
    s.pad_now = &pad_now_fn;
    s.padman = &padman;
    s.sd_block_trans = &sd_block_trans;
    s.sdr_remote = &sdr_remote;
    s.pad_replay = &pad_replay;
    s.pad_input_source = &pad_input_source;
    s.pad_stamp = &pad_stamp;
    return ArmPlayMode(s);
}
