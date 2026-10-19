// Phase functions of the composition root (EeBootstrap::Run split, ARCHITECTURE section 11):
// each phase unpacks the upstream objects it uses from LegState, constructs its own, registers
// the ones later phases need and tail-calls the next phase, so object lifetimes nest exactly as
// the former single scope did (later phases' objects are destroyed first).
#include "games/slus_20909_crash_twinsanity/composition/leg_state.h"
#include "engine/platform/host/platform.h"

#include <cstring>

int EeBootstrap::ArmMemoryAndClocks(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    const bool play_mode = !cfg.harness.headless && !cfg.time.tick_sync;
    // --- memory + MMIO (sink first; INTC/timers displace it below) -------------
    MainMemory ram;
    Scratchpad spad;
    AddressMap map(ram, spad);
    MetaArena arena;
    arena.Register(map);
    MmioSink sink;
    MmioRouter mmio(sink);
    FastMem fast_mem(ram, map, mmio);
    RecompAbi::BindMemory(&fast_mem);
    // --dump-frames <dir> (a harness knob, not a Config key): every build writes the
    // frames lane's image of each present as <dir>/f<key>.ppm (census dump_frames=).
    ps2r::app::SessionFiles session_files(ps2r::ConfigLoader::Request().dump_frames_dir);
    const std::string& dump_frames_dir = session_files.FrameDirectory();
    uint64_t dump_frames_written = 0;
#if PS2R_DIAGNOSTICS
    // --watch-word (a harness knob, not a Config key): diagnostics builds install
    // the WordWatch census as FastMem's write observer, so every CPU store to a
    // named guest word is attributed to its recompiled function (<leg>/watch.log).
    ps2r::diag::WordWatch& word_watch = ps2r::diag::WordWatch::Instance();
    word_watch.Clear();
    {
        const std::string& list = ps2r::ConfigLoader::Request().watch_words;
        std::string watch_err;
        if (!list.empty() && !word_watch.Parse(list, &watch_err)) {
            std::fprintf(stderr, "nativeee: %s\n", watch_err.c_str());
            return 2;
        }
        // The census is flushed at the leg's end; a leg that spins inside one
        // frame never gets there, so the observer also rewrites <leg>/watch.log
        // every kWatchFlushEvery recorded stores (diagnostics only; the file is
        // the whole census each time, never a guest-visible effect).
        if (word_watch.Armed()) {
            WatchFlush().flush_path = cfg.harness.out_dir.empty() ? std::string() : cfg.harness.out_dir + "/watch.log";
            WatchFlush().flush_from = ps2r::ConfigLoader::Request().watch_flush_from;
            WatchFlush().record_from = ps2r::ConfigLoader::Request().watch_record_from;
            WatchFlush().record_on = WatchFlush().record_from == 0;
            FastMem::SetWriteObserver(
                [](void* user, uint32_t addr, unsigned width, uint64_t old_bits, uint64_t new_bits) {
                    constexpr uint64_t kWatchFlushEvery = 64;
                    if (!WatchFlush().record_on) return;
                    auto* ww = static_cast<ps2r::diag::WordWatch*>(user);
                    const uint64_t before = ww->Hits();
                    ps2r::diag::WordWatch::OnWrite(user, addr, width, old_bits, new_bits);
                    if (WatchFlush().flush_on && ww->Hits() != before && ww->Hits() % kWatchFlushEvery == 0 &&
                        !WatchFlush().flush_path.empty()) {
                        ww->Flush(WatchFlush().flush_path);
                        if (WatchFlush().census) WatchFlush().census();
                    }
                },
                &word_watch, word_watch.RangeLo(), word_watch.RangeHi());
        }
        ps2r::diag::DiagRegistry::Instance().Register(
            "watch_word", "CPU stores to the --watch-word guest words, attributed to the recompiled writer (<leg>/watch.log)",
            word_watch.Armed());
    }
    // --vu0-trace (a harness knob, not a Config key): diagnostics builds record
    // the COP2 register block before and after every VCALLMS of the named VU0
    // micro pcs (<leg>/vu0_trace.log); bound to the macro bridge below.
    Vu0Trace vu0_trace;
    {
        const std::string& list = ps2r::ConfigLoader::Request().vu0_trace;
        std::string trace_err;
        if (!list.empty() && !vu0_trace.Parse(list, &trace_err)) {
            std::fprintf(stderr, "nativeee: %s\n", trace_err.c_str());
            return 2;
        }
        ps2r::diag::DiagRegistry::Instance().Register(
            "vu0_trace", "COP2 block in/out of every VCALLMS of the --vu0-trace micro pcs (<leg>/vu0_trace.log)",
            vu0_trace.Armed());
    }
#endif

    // --- time domain, boundary owner, INTC + timers ---------------------------
    Intc& intc = Intc::Instance();
    intc.Register(mmio);
    EmuDomain domain;
    if (cfg.time.tick_deterministic) domain.SetRegimePin(1);  // Pinned regime under the deterministic profile
    DerivedClocks clocks(domain);
    clocks.SetRtcFixed(cfg.game.rtc_fixed);
    SetLibscfClocks(&clocks);   // the libscf sceScfGetLocalTimefromRTC row reads this RTC
    EeTimers timers(clocks, intc);
    timers.Register(mmio);
    // Phase 4C: the SIF register block and the DMAC register page displace the
    // sink (the sceSifSetReg/GetReg syscalls read and write the SIF words
    // through the router). SIF payload movement is the syscall/sifman memcpy
    // (4C'), never DMAC register traffic.
    SifState sif_state;
    sif_state.Register(mmio);
    timers.SetCtxProvider([] { return ThreadManager::Instance().Current() ? ThreadManager::Instance().Current()->ctx : nullptr; });
    TickSource tick_source;
    tick_source.SetPolicy(cfg.time.tick_deterministic ? TickSource::Policy::Deterministic : TickSource::Policy::Play);
    tick_source.SetMode(cfg.harness.headless ? TickSource::Mode::Freerun : TickSource::Mode::Realtime);
    FrameRendezvous rendezvous;
    std::atomic<bool> running{true};
    s.play_mode = &play_mode;
    s.ram = &ram;
    s.spad = &spad;
    s.map = &map;
    s.arena = &arena;
    s.sink = &sink;
    s.mmio = &mmio;
    s.session_files = &session_files;
    s.dump_frames_dir = &dump_frames_dir;
    s.dump_frames_written = &dump_frames_written;
#if PS2R_DIAGNOSTICS
    s.word_watch = &word_watch;
    s.vu0_trace = &vu0_trace;
#endif
    s.intc = &intc;
    s.domain = &domain;
    s.clocks = &clocks;
    s.timers = &timers;
    s.sif_state = &sif_state;
    s.tick_source = &tick_source;
    s.rendezvous = &rendezvous;
    s.running = &running;
    return ArmGraphicsPipeline(s);
}

// --vu1-miss-cases <dir> (a harness knob, not a Config key): both VU1 HLE runtimes
// write the first kicks of every missed (variant, entry) as Vu1Case files -- the
// replay oracle's input when a route misses the library.
static void ArmVu1MissCapture(Vu1HleRuntime& inline_rt, Vu1HleRuntime& worker_rt) {
    const std::string& miss_dir = ps2r::ConfigLoader::Request().vu1_miss_cases_dir;
    if (miss_dir.empty()) return;
    std::filesystem::create_directories(miss_dir);
    inline_rt.SetMissCapture(miss_dir, "inline", 4);
    worker_rt.SetMissCapture(miss_dir, "worker", 4);
}

int EeBootstrap::ArmGraphicsPipeline(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    const bool& play_mode = *s.play_mode;
    ps2r::verify::VerifyHost& host = *s.host;
    const TitleProfile& title = *s.title;
    MainMemory& ram = *s.ram;
    Scratchpad& spad = *s.spad;
    AddressMap& map = *s.map;
#if PS2R_DIAGNOSTICS
    const std::string& dump_frames_dir = *s.dump_frames_dir;
    Vu0Trace& vu0_trace = *s.vu0_trace;
#endif
    std::atomic<bool>& running = *s.running;
    // --- GS state machine ---------------------------------------------
    // GsState owns the register file, the vertex kick, THE flush, the transfer
    // engine, the privileged registers and the VRAM mirror. Its DrawIr consumer
    // is the counting sink (its chained DrawRec hash is the DrawIR observable of
    // a leg: run.meta drawir= and drawir.ohash) decorating the queue consumer,
    // which lowers the stream into the live video stack's DrawPipeline when a GL
    // context exists (video=gl); without one the counting sink is the end of the
    // chain and the present stays the stub's black frame (video=stub). The
    // privileged-register jobs already carry the LIVE VramImage upload epoch
    // (GsState's own SetJobContext). The sprite-corner rule is title data.
    ps2r::gs::GsState gs_state;
    gs_state.SetQuirks(ps2r::gs::VertexKickQuirks{title.quirks.round_sprite_mode});
    ps2r::app::VideoStack video;
    // gs.queue=pipelined (the split image): the pipeline runs on
    // ITS OWN VramImage on the consumer thread; the EE keeps GsState's image for
    // its writes, marks and upload epoch, and every EE-side call on it is logged
    // (VramMirror) and replayed on the pipeline's image in stream order by
    // the queue -- the in-thread interleaving without the shared image. The EE's
    // reads of pipeline-side state (a local->local copy's source, the savestate's
    // VRAM section) drain the queue and read the pipeline's image.
    const bool gs_queue_split_image = cfg.video.backend == ps2r::VideoBackend::Gl &&
                                      cfg.gs.queue == ps2r::GsQueue::Pipelined;
    std::unique_ptr<ps2r::gs::VramImage> pipeline_vram;
    if (gs_queue_split_image) pipeline_vram = std::make_unique<ps2r::gs::VramImage>();
    const bool video_live = cfg.video.backend == ps2r::VideoBackend::Gl &&
                            video.Create(cfg.video, cfg.harness.headless, "NativeEE",
                                         pipeline_vram ? *pipeline_vram : gs_state.Vram());
#if PS2R_DIAGNOSTICS
    const TexresTraceRequest texres_req = ReadTexresTraceRequest(dump_frames_dir);
    if (texres_req.enabled && video.Active()) video.Pipe()->Resolver().Trace().SetFilter(texres_req.filter);
#endif
    std::unique_ptr<ps2r::gs::DrawIrQueueConsumer> consumer;
    // gs.queue=thread (plan row 6.3): the DrawIR ring carries the stream to the
    // consumer on its own host thread, which owns the GL context (the EE thread
    // never draws). Lockstep: each push returns once consumed, so the producer /
    // consumer interleaving over the shared VRAM image is the inline one and the
    // frames lane is byte-identical by construction (the pin). The payload and
    // the CRTC snapshot are captured at the push, on the EE thread.
    std::unique_ptr<ps2r::gs::DrawIrThreadQueue> gs_queue;
    const bool gs_queue_thread = video_live && cfg.gs.queue != ps2r::GsQueue::Inline;
    if (video_live) {
        // The display grab's source when no target sits at the scanout base
        // (the one-line toggle): LocalMemory decodes the DISPFB buffer out
        // of local memory -- the CRTC's read; a cold boot's first 61 presents then
        // hash as the black screen the console shows, where the original's lines
        // read 0 ("no readable scanout") -- TargetFallback (the original's pick,
        // the default until the frames lane pairs that class) falls back to the
        // most recently drawn target. On the anchors neither arm engages: the
        // title draws its display buffer before the first vblank.
        video.Pipe()->Scanout().SetGrabPolicy(ps2r::gs::ScanoutComposer::GrabPolicy::TargetFallback);
        ps2r::gs::DrawIrQueueConsumer::Hooks hooks;
        hooks.enter = [&video] { video.EnsureCurrent(); };
        hooks.owned_frame = [&video](bool frame, auto* out) { return video.PresentOwnedFrame(frame, out); };
        auto crtc_now = [&gs_state] {
            using P = ps2r::gs::GsPrivRegs;
            const P& p = gs_state.Priv();
            return ps2r::gs::CrtcClassifier::Classify(p.Read(P::kPmode), p.Read(P::kDispfb1), p.Read(P::kDispfb2),
                                                      p.Read(P::kDisplay1), p.Read(P::kDisplay2));
        };
        if (gs_queue_thread) {
            hooks.upload_pixels = [&gs_queue] { return gs_queue->LastUploadPayload(); };
            hooks.crtc = [&gs_queue] { return gs_queue->CurrentFrameCrtc(); };
        } else {
            hooks.upload_pixels = [&gs_state] { return &gs_state.Xfer().LastUploadPixels(); };
            hooks.crtc = crtc_now;
        }
        // The window geometry call is main-thread only (SDL/Cocoa): the consumer
        // thread applies the device viewport and leaves the OS resize to slot 0.
        if (gs_queue_thread) hooks.window_resized = [&video](int w, int h) { video.OnWindowResizedOffMain(w, h); };
        else hooks.window_resized = [&video](int w, int h) { video.OnWindowResized(w, h); };
        // The consumer half of the identity stamp: a stamped draw's handler id
        // (GsdlFormat::HandlerId over the library entry's name -- the inverse of
        // the mapping the identity observer applies below) resolves back to the
        // entry's name, which the class classifier matches against the title's
        // `stamps` substrings; a nameless stamped draw classifies as hle-other
        // (no renderer -> the generic path), so this table is what routes HLE'd
        // draws to their class renderers.
        {
            const Vu1ProgramLibrary& lib = Vu1Library::Get();
            std::vector<std::pair<uint32_t, const char*>> names;
            names.reserve(lib.entry_count);
            for (size_t i = 0; i < lib.entry_count; ++i)
                names.emplace_back(ps2r::gs::GsdlFormat::HandlerId(lib.entries[i].name), lib.entries[i].name);
            hooks.handler_name = [names](uint32_t id) -> const char* {
                for (const auto& n : names)
                    if (n.first == id) return n.second;
                return nullptr;
            };
        }
        consumer = std::make_unique<ps2r::gs::DrawIrQueueConsumer>(*video.Pipe(), std::move(hooks));
        // The frames-lane hash is taken where the grab is made, at the present
        // scale the boundary would pass (video.Scale()): the wall-locked play
        // boundary reads the hash, not the pixels (no 3.7 MB copies per
        // frame on the game thread).
        consumer->SetFrameHashScale(video.Scale());
        if (gs_queue_thread) {
            ps2r::gs::DrawIrThreadQueue::Hooks qh;
            qh.upload_pixels = [&gs_state] { return &gs_state.Xfer().LastUploadPixels(); };
            qh.crtc = crtc_now;
            const bool lockstep = cfg.gs.queue == ps2r::GsQueue::Thread;
            gs_queue = std::make_unique<ps2r::gs::DrawIrThreadQueue>(
                *consumer, std::move(qh),
                lockstep ? ps2r::gs::DrawIrThreadQueue::Policy::Lockstep : ps2r::gs::DrawIrThreadQueue::Policy::Pipelined,
                4096, pipeline_vram.get());
            if (pipeline_vram) {
                gs_state.Vram().SetMirror(gs_queue->ProducerMirror());
                // The local->local copy's source. Deterministic (no frame skip): drain
                // the queue and bring the pipeline image in wholesale -- the identity
                // barrier. Play (the clock contract, rule 2): never wait -- the EE image
                // is authoritative for what the EE wrote; a GPU target's pixels come
                // from the consumer's last completed frame, staleness counted
                // (gs_queue l2l_*=).
                gs_state.Xfer().SetLocalToLocalPrepare(
                    [&gs_queue, &pipeline_vram](ps2r::gs::VramImage::Span source, ps2r::gs::VramImage& image) {
                        if (gs_queue->FrameSkip()) return gs_queue->ServeLocalToLocalSource(source, image);
                        gs_queue->Sync();
                        std::memcpy(image.MutableBytes(), pipeline_vram->Bytes(), ps2r::gs::VramImage::kBytes);
                        uint32_t first, count;
                        ps2r::gs::VramImage::PagesOf(source, &first, &count);
                        for (uint32_t i = 0; i < count; ++i)
                            if (pipeline_vram->Page(first + i).author == ps2r::gs::PageRecord::Author::GpuTarget) return true;
                        return false;
                    });
            }
            // Play mode (the clock contract, rule 2): the renderer never blocks the
            // game -- one pending frame, the older dropped (frame-skip); the
            // deterministic profile keeps the lockstep/pipelined identity (no drop).
            gs_queue->SetFrameSkip(play_mode && !lockstep);
            video.ReleaseCurrent();
            // The consumer is on the EE's critical path (every boundary waits for
            // its frame; every owned group waits for its slot): the same
            // interactive QoS the MTVU worker and the presenter thread run at.
            gs_queue->Start([&video] {
                ps2r::platform::host::PinCurrentThreadInteractive("ps2r-gs", ps2r::host::ThreadRole::Graphics);
                video.EnsureCurrent();
            });
            PS2R_LOG(HOST, Info, "GS queue: consumer thread started (%s%s)", lockstep ? "lockstep" : "pipelined",
                     pipeline_vram ? ", split VRAM image" : "");
        }
    }
    // The head of the DrawIR chain behind the counting sink (and the recorders).
    ps2r::gs::DrawIrSink* const gs_sink_head =
        gs_queue ? static_cast<ps2r::gs::DrawIrSink*>(gs_queue.get()) : static_cast<ps2r::gs::DrawIrSink*>(consumer.get());
    // Handler names resolve as the consumer's table above does (GsdlFormat::HandlerId
    // over the library entry's name) for both recorders below.
    const auto handler_name = [](uint32_t id) -> const char* {
        const Vu1ProgramLibrary& lib = Vu1Library::Get();
        for (size_t i = 0; i < lib.entry_count; ++i)
            if (ps2r::gs::GsdlFormat::HandlerId(lib.entries[i].name) == id) return lib.entries[i].name;
        return nullptr;
    };
    // The bounded drawlist ring (every build): the last 200 frames of the DrawIR
    // stream, the crash path's drawlist_last200.txt (TriageBundle). Fixed size,
    // allocated once -- and in the chain right before the
    // consumer, so it holds exactly what the consumer rendered.
    ps2r::gs::DrawlistRing drawlist_ring(gs_sink_head, handler_name);
#if PS2R_DIAGNOSTICS
    // The .gsdl drawlist recorder: diagnostics builds record the
    // DrawIR stream to <harness out dir>/drawlist.gsdl whenever the harness is on --
    // no Config key; the chain is counting sink -> recorder -> ring -> consumer, so
    // the file holds exactly what the consumer rendered.
    ps2r::diag::DrawlistRecorder drawlist_recorder(&drawlist_ring, handler_name);
    if (!cfg.harness.out_dir.empty()) drawlist_recorder.Open(cfg.harness.out_dir + "/drawlist.gsdl");
    ps2r::diag::DiagRegistry::Instance().Register(
        "drawlist", "DrawIR stream recorded as <harness out dir>/drawlist.gsdl (GsdlFormat v2, vertices included)",
        drawlist_recorder.IsOpen());
    ps2r::gs::CountingDrawIrSink draw_sink(&drawlist_recorder);
#else
    ps2r::gs::CountingDrawIrSink draw_sink(&drawlist_ring);
#endif
    gs_state.SetSink(&draw_sink);
    // --- GIF / VU / VIF / DMAC, in the checklist's construction order
    // The parser's sink fans out to the GS façade (GsStateSink) AND the
    // RegisterOpStream recorder (its FNV-1a hash is the GS-side observable of
    // every GIF stream: run.meta gsops_hash / gsops.ohash, compared against the
    // façade's draws at the gate); the arbiter's boundary hook is the presenter
    // push.
    GifRegs gif_regs;
    // Hash-only: the leg consumes Hash()/Count() alone; a retaining recorder grows
    // 24 bytes per sink call for the whole leg (the title's uploads and vertices
    // made that ~8-11 MB per frame
    RegisterOpStream gs_ops(/*retain_records=*/false);
    GsStateSink gs_state_sink(gs_state);
    // The recorder is the GATE's instrument: the deterministic profile's gsops
    // lane is diffed against a reference leg. The play profile has no consumer
    // for it and paid ~13 % of a level frame folding every vertex and IMAGE
    // quadword into the hash (the s3 performance round), so the recorder joins
    // the fan-out under the deterministic profile only; run.meta names the
    // choice (gsops_recorder=) and the play lane's gsops_hash reads `off`.
    const bool gsops_recorder = cfg.time.tick_deterministic;
    GsSinkFanout gif_sink({&gs_state_sink});
    if (gsops_recorder) gif_sink.Add(&gs_ops);
    GifParser gif_parser(gif_sink, gif_regs);
    GifArbiter gif_arbiter(gif_parser, nullptr);
    VuUnit vu0(0);
    VuUnit vu1(1, &gif_arbiter, title.quirks.xgkick_defer);
    // engine.vu1_mode -> the fallback tier: Hle -> Cached, Cached -> Cached,
    // Interp -> Interp (the enum's Hle value names the same tier as Cached).
    // hle_enabled = (library != none): the ONLY library this root links is the
    // title's; `--vu1-library none` (a harness knob, not a Config key)
    // turns the consult off: the HLE hook is not set, the registry is
    // effectively empty and every image runs on the fallback tier (PHASE5 (e)
    //Run.meta vu1_library names what ran.
    const bool vu1_library_none = ps2r::ConfigLoader::Request().vu1_library_none;
    {
        VuSettings& vs = vu1.dispatch().settings();
        vs.vu1_mode = cfg.engine.vu1_mode == ps2r::Vu1Mode::Interp ? VuTierMode::Interp : VuTierMode::Cached;
        vs.hle_enabled = !vu1_library_none;
    }
    if (auto* meta = host.Meta()) {
        meta->Set("vu1_library", vu1_library_none ? "none" : "title");
        meta->Set("vu1_tier", cfg.engine.vu1_mode == ps2r::Vu1Mode::Interp ? "interp" : "cached");
    }
    KickLowering kick_lowering(gif_arbiter, gif_sink);
    Vu1HleRuntime vu1_hle(Vu1Library::Get(), kick_lowering);
    if (!vu1_library_none) vu1.dispatch().SetHleHook(&Vu1HleRuntime::Hook, &vu1_hle);
    // The DrawIR identity stamp (invariant 28): the HLE runtime reports each
    // kick's {handler, image, entry pc, ordinal} before its submit; the GS sink
    // carries it across the arbiter queue (one slot per PATH1 packet) and
    // applies SetIdentity / ClearIdentity as the drain delivers the packet. The
    // handler id is GsdlFormat::HandlerId over the entry's name -- the numbering
    // the .gsdl corpus and the class classifier already use.
    gif_arbiter.SetSubmitObserver(&GsStateSink::OnArbiterSubmit, &gs_state_sink);
    gif_arbiter.SetDrainObserver(&GsStateSink::OnArbiterDeliver, &gs_state_sink);
    vu1_hle.SetIdentityObserver([](void* user, const Vu1HleRuntime::KickIdentity* id) {
        auto* sink = static_cast<GsStateSink*>(user);
        if (!id) {
            sink->ClearStagedIdentity();
            return;
        }
        sink->StageKickIdentity(ps2r::gs::GsdlFormat::HandlerId(id->handler_name), id->image_hash, id->kick_pc,
                                id->kick_ord);
    }, &gs_state_sink);
    Vu0Registry vu0_registry;
    Vu0Programs::Register(vu0_registry);
    Vif vif(vu0, vu1, gif_arbiter, gif_regs);
    // The worker's library consult (engine.mtvu): its own lowering and runtime
    // on the worker's unit; the VIF1 TOPS observer feeds both runtimes (one
    // VIF1, whichever unit runs the program), the identity observer stages into
    // the same GS sink slot the arbiter submit observer reads on the submitting
    // thread. `--vu1-library none` turns both consults off.
    KickLowering worker_kick_lowering(gif_arbiter, gif_sink);
    Vu1HleRuntime worker_vu1_hle(Vu1Library::Get(), worker_kick_lowering);
    ArmVu1MissCapture(vu1_hle, worker_vu1_hle);
    struct VifTopsTargets { Vu1HleRuntime* inline_rt; Vu1HleRuntime* worker_rt; };
    VifTopsTargets tops_targets{&vu1_hle, &worker_vu1_hle};
    vif.SetTopsObserver([](void* user, uint16_t tops, uint16_t itops) {
        auto* t = static_cast<VifTopsTargets*>(user);
        t->inline_rt->SetVifTops(tops, itops);
        t->worker_rt->SetVifTops(tops, itops);
    }, &tops_targets);
    Dmac dmac(ram, spad, vif, gif_arbiter);
    ps2r::app::GraphicsOutput graphics_output(ram, gif_parser, gif_arbiter, gs_state,
                                            dmac.walker(), gs_queue.get(), cfg.hle.graphics_strips);
    if (video_live && (!consumer || !graphics_output.AttachVideo(video, *consumer))) return 2;
    // The clock contract (rule 2): in play mode the owned deliveries run on the GS
    // consumer without the game thread waiting (the queue's frame skip is on).
    graphics_output.SetAsyncOwnedDelivery(gs_queue && gs_queue->FrameSkip());
    // The walked chain's bucket, for the sink's PATH1-by-bucket attribution row
    // (exact under the inline VIF1 feed only; observation, nothing reordered).
    graphics_output.SetBucketHint([&gs_state_sink](int bucket) { gs_state_sink.SetBucketHint(bucket); });
    const ps2r::app::EffectsApplication::Selection effects_selected{cfg.hle.graphics_shadows, cfg.hle.graphics_distortion,
                                                                    cfg.hle.graphics_screen};
    if ((effects_selected.shadows || effects_selected.distortion || effects_selected.screen) && (!video_live || !consumer))
        return 2;
    ps2r::app::EffectsApplication effects_application(ram, video_live ? &video : nullptr, effects_selected, {
        // Main RAM and the META arena (the DiskManager nodes a shadow model's
        // bytes are found through live there under hle.meta_arena routing).
        [&](uint32_t at, void* out, size_t size) {
            const auto* bytes = map.Translate(at, size);
            if (!bytes || !out) return false;
            std::memcpy(out, bytes, size);
            return true;
        },
        [&](uint32_t bucket, auto& frame) { return graphics_output.CaptureScene(bucket, frame); },
        // Bucket-23 source environment: the completed frame's tuple captured
        // producer-side by GraphicsOutput::CaptureEffects (buckets 4/21/23).
        [&](::title::MaterialState::Environment& environment) {
            ::title::FrameEnvironment::SceneFrame frame;
            return graphics_output.CaptureEffects(23, frame, environment);
        },
        [&](uint32_t bucket, ps2r::app::EffectsApplication::Work work) {
            return graphics_output.RecordOwned(bucket,
                [work = std::move(work)](ps2r::app::VideoStack&) { return work(); });
        },
        [&](ps2r::app::EffectsApplication::Work work) {
            if (!consumer) return false;
            const auto ordered = [&] {
                return consumer->RunOrdered([&] { video.EnsureCurrent(); return work(); });
            };
            return gs_queue ? gs_queue->RunOrdered(ordered) : ordered();
        },
        [&](std::string* reason) { return graphics_output.CanCheckpointScene(reason); },
        // The capture-time settle (Graphics INTERFACES section 12): an open
        // recording at the boundary admits the effects' closed passes.
        [&] { return graphics_output.SettleOwnedForCapture(nullptr) && graphics_output.UnlinkedAtBoundary().items != 0; },
        // The cloth producers' execution census (graphics_cloth_producer=): the audited counting wrapper.
        [](uint32_t fn, DispatchTable::Body body, std::shared_ptr<std::atomic<uint64_t>> counter) {
            return DispatchTable::CountCalls(fn, std::move(body), std::move(counter));
        }
    });
    VuMacroBridge::Bind(&vu0, &vu1, &vu0_registry);
#if PS2R_DIAGNOSTICS
    if (vu0_trace.Armed()) VuMacroBridge::SetTrace(&vu0_trace);
#endif
    // The MTVU half (the worker's VU1 unit, runahead guard + backpressure, the
    // worker, MtvuSync on the inline dispatch, the DMAC staged feed, the GIF
    // drain thread) exists ONLY under engine.mtvu: the deterministic profile
    // sets it off and a harness leg constructs none of it (its lanes are the
    // inline path's). engine.mtvu_depth is the runahead cap in frames. The
    // threads start here, before any producer traffic (the ELF is not loaded
    // yet); they stop at the stack's destruction, after the guest threads.
    std::unique_ptr<MtvuStack> mtvu;
    if (cfg.engine.mtvu) {
        MtvuSettings mtvu_settings;
        mtvu_settings.enabled = true;
        mtvu_settings.depth_frames = cfg.engine.mtvu_depth;
        mtvu = std::make_unique<MtvuStack>(vif, dmac.walker(), gif_arbiter, vu1.dispatch(), vu1.dispatch().settings(),
                                           title.quirks.xgkick_defer, mtvu_settings, &running);
        if (!vu1_library_none) {
            // The title library on the worker's unit: the same consult the inline
            // unit has, from the worker thread, so a play frame's VU1 programs run
            // native instead of on the cached tier (run.meta vu1_hle_worker=).
            worker_vu1_hle.SetIdentityObserver([](void* user, const Vu1HleRuntime::KickIdentity* id) {
                auto* sink = static_cast<GsStateSink*>(user);
                if (!id) {
                    sink->ClearStagedIdentity();
                    return;
                }
                sink->StageKickIdentity(ps2r::gs::GsdlFormat::HandlerId(id->handler_name), id->image_hash,
                                        id->kick_pc, id->kick_ord);
            }, &gs_state_sink);
            mtvu->WorkerVu1().dispatch().SetHleHook(&Vu1HleRuntime::Hook, &worker_vu1_hle);
            mtvu->WorkerVu1().dispatch().settings().hle_enabled = true;
        }
        // The GIF drain thread would run the parser -> GsState -> the DrawIR
        // consumer, which draws on the draining thread when the video stack is
        // live; a GL context is current on one thread only, so the drain stays
        // inline on the EE thread while GL is live.
        // The clock contract's kick drop goes with the GS queue's frame skip
        // (play mode, not lockstep): the game thread never parks on the worker.
        mtvu->SetKickDrop(play_mode && cfg.gs.queue != ps2r::GsQueue::Thread && cfg.engine.mtvu_kick_drop);
        mtvu->Start(/*with_drain_thread=*/!video_live);
        PS2R_LOG(HOST, Info, "MTVU: worker started, drain %s (depth_frames=%d)", video_live ? "inline" : "thread",
                 cfg.engine.mtvu_depth);
    }
    if (auto* meta = host.Meta()) meta->Set("mtvu", mtvu ? mtvu->Census() : "off");
    s.gs_state = &gs_state;
    s.video = &video;
    s.video_live = &video_live;
#if PS2R_DIAGNOSTICS
    s.texres_req = &texres_req;
#endif
    s.consumer = &consumer;
    s.gs_queue = &gs_queue;
    s.pipeline_vram = &pipeline_vram;
#if PS2R_DIAGNOSTICS
    s.drawlist_recorder = &drawlist_recorder;
#endif
    s.draw_sink = &draw_sink;
    s.drawlist_ring = &drawlist_ring;
    s.gif_regs = &gif_regs;
    s.gs_ops = &gs_ops;
    s.gsops_recorder = gsops_recorder;
    s.gs_state_sink = &gs_state_sink;
    s.gif_parser = &gif_parser;
    s.vu0 = &vu0;
    s.vu1 = &vu1;
    s.kick_lowering = &kick_lowering;
    s.vu1_hle = &vu1_hle;
    s.worker_vu1_hle = &worker_vu1_hle;
    s.worker_kick_lowering = &worker_kick_lowering;
    s.vif = &vif;
    s.dmac = &dmac;
    s.graphics_output = &graphics_output;
    s.effects_application = &effects_application;
    s.mtvu = &mtvu;
    return ArmIopAndMedia(s);
}
