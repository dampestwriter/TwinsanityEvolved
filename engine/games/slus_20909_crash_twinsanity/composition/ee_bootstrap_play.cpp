// Phase functions of the composition root (EeBootstrap::Run split, ARCHITECTURE section 11):
// each phase unpacks the upstream objects it uses from LegState, constructs its own, registers
// the ones later phases need and tail-calls the next phase, so object lifetimes nest exactly as
// the former single scope did (later phases' objects are destroyed first).
#include "games/slus_20909_crash_twinsanity/composition/leg_state.h"

int EeBootstrap::ArmPlayMode(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    ps2r::verify::VerifyHost& host = *s.host;
    const EeBootstrap::Paths& paths = *s.paths;
    const bool& play_mode = *s.play_mode;
    MainMemory& ram = *s.ram;
    Scratchpad& spad = *s.spad;
    MetaArena& arena = *s.arena;
    EmuDomain& domain = *s.domain;
    TickSource& tick_source = *s.tick_source;
    ps2r::app::VideoStack& video = *s.video;
    const bool& video_live = *s.video_live;
#if PS2R_IOP_ORACLE
    IopModuleTable& iop_table = *s.iop_table;
    ps2r::iop::IopMemory& iop_mem = *s.iop_mem;
    ps2r::title::stream_irx::StreamIrxPlugin& stream_irx = *s.stream_irx;
    ps2r::title::stream_irx::StreamRpcServer& stream_server = *s.stream_server;
#endif
    const bool& iso_open = *s.iso_open;
    ReadEngine& read_engine = *s.read_engine;
    ps2r::app::SoundOutput& sound_output = *s.sound_output;
    BoundarySequencer& sequencer = *s.sequencer;
    DeliveryMeter& delivery_meter = *s.delivery_meter;
    LoaderModel& loader_model = *s.loader_model;
    std::ofstream& loader_requests = *s.loader_requests;
    RpcRouter& rpc_router = *s.rpc_router;
    PadProtocol& pad_protocol = *s.pad_protocol;
    ps2r::triage::InputLog& pad_replay = *s.pad_replay;
    std::string& pad_input_source = *s.pad_input_source;
    PadStampDuty& pad_stamp = *s.pad_stamp;
    // Registered by a LATER phase (RegisterSections, after the image load):
    // read through the state at run time, never bound here.
    LegState* const leg_state = &s;
    // Play mode = a window exists and the boundary is wall-paced (neither
    // --headless nor time.tick_sync): the ONLY mode that opens an audio device
    // or reads the host's input devices. Harness legs (deterministic profile,
    // headless) take neither branch, so their lanes are unchanged.
    // Pad source precedence: a loaded replay script > the live host input
    // (play mode) > the neutral pad. The live source is sampled on the boundary
    // and stamped through the same PadProtocol::Stamp; when recording, the
    // served sample feeds the harness's incremental input recorder.
    // The recorder's key is the GAME FRAME: the frames-lane key of the boundary
    // being served (PresentKey of the boundary the Verify slot closes a few
    // slots later -- the title reads the pad once per boundary). The logical
    // tick is NOT the key: in play it is wall-paced (a late boundary consumes
    // the elapsed fields, a pace park the fields it waited), so a tick-keyed
    // recording of a play session named ticks the deterministic replay reached
    // at other frames (the s4 recording sat on the load menu). A replay script
    // is indexed by the clock its header names (InputLog::Key).
    std::unique_ptr<PlatformInput> live_input;
    if (play_mode && pad_input_source != "replay") {
        live_input = std::make_unique<PlatformInput>(PlatformInput::SettingsFrom(cfg, paths.exe_dir));
        pad_input_source = "live";
    }
    // harness.live_pad_probe: the replay serves, AND the live pad is opened and
    // read at every boundary as a live session reads it (RootScreenModeSwap: the
    // pad's round trip to main is the product path a display swap must not
    // block). `live_input` stays null for every other consumer (the served
    // sample, the movie skip, the rumble serve); only SampleInput probes it.
    std::unique_ptr<PlatformInput> pad_probe;
    if (play_mode && cfg.harness.live_pad_probe && pad_input_source == "replay") {
        pad_probe = std::make_unique<PlatformInput>(PlatformInput::SettingsFrom(cfg, paths.exe_dir));
        pad_input_source = "replay+probe";
    }
    // A5 (qol.skip_movies): START (bit 3) or CROSS (bit 14) pressed on the live
    // pad while the owned Movie plays -> the owned Movie stop (MovieDecoder::
    // RequestSkip: the title's own Step sees the end and runs its Stop path; the
    // title's CROSS skip of a skippable movie stays its own). Edge-triggered.
    // Checked at the boundary's pad serve AND at the FMV pace park's display
    // ticks (a movie's 547 pictures pass inside ~10 boundaries: the pad must be
    // read while the EE is parked). Play mode with live input only -- no lane
    // of a deterministic leg changes.
    bool skip_was_down = false;
    uint64_t skip_ticks = 0;
    auto movie_skip_check = [&](uint16_t buttons_active_high, uint64_t frame) {
        if (!cfg.qol.skip_movies || !live_input || !leg_state->game_libraries) return;
        const bool pressed = (buttons_active_high & ((1u << 3) | (1u << 14))) != 0;
        if (pressed && !skip_was_down && leg_state->game_libraries->Movie().Playing()) {
            leg_state->game_libraries->Movie().RequestSkip();
            std::fprintf(stderr, "nativeee: qol.skip_movies: movie skipped at frame %llu\n", (unsigned long long)frame);
        }
        skip_was_down = pressed;
    };
    // The park-time poll: every 4th parked field, only while the owned Movie plays.
    std::function<void()> movie_skip_poll = [&] {
        if (!cfg.qol.skip_movies || !live_input || (++skip_ticks & 3) != 0 || !leg_state->game_libraries ||
            !leg_state->game_libraries->Movie().Playing()) return;
        const PadProtocol::Input raw = live_input->Sample();
        movie_skip_check(raw.buttons, skip_ticks);
    };
    s.movie_skip_poll = &movie_skip_poll;
    // A5 (qol.fast_forward = N): R2 held on the live pad while the title's video
    // controller plays a cutscene -> the wall-locked boundary delivers N fields
    // instead of one (the late-frame case: the title's clock
    // steps the cutscene N frames per boundary; the boundary keeps its 60 Hz
    // wall edge, so game_hz is unchanged). The buttons are the served ones (live
    // or a replayed padlog). Off (factor 1) outside play mode: the sequencer is
    // never given a provider (the deterministic boundary never asks for one).
    ::title::CutsceneFastForward fast_forward(play_mode ? cfg.qol.fast_forward : 1,
        [&ram](uint32_t a, uint32_t* v) {
            const uint8_t* p = ram.Pointer(MainMemory::Phys(a), 4);
            if (!p) return false;
            *v = memory::read<uint32_t>(a);
            return true;
        });
    if (fast_forward.Enabled()) sequencer.SetExtraFields([&fast_forward] { return fast_forward.ExtraFields(); });
    s.fast_forward = &fast_forward;
    using InputSave = ps2r::app::InputSave;
    const uint32_t input_save_selection =
        (cfg.hle.input_service ? InputSave::Checkpoint::kInput : 0) |
        (cfg.hle.save_storage ? InputSave::Checkpoint::kSaveStorage : 0);
    InputSave input_save(input_save_selection, InputSave::RamAccess(ram, spad),
        {cfg.paths.mc_dir}, [&] { return domain.DomainTick(); },
        [&](unsigned port, unsigned slot, ::title::Input::Motors motors) {
            // input.rumble off: the request is accepted (the title sees its motors
            // served) but nothing reaches the controller.
            return port == 0 && slot == 0 && live_input && (!cfg.input.rumble || live_input->QueueMotors(motors.small, motors.big));
        });
    // The Display page's rows bound to the video stack:
    // constructed with the session below; the boundary applies a pending scale
    // on the backend owner and feeds the camera track once Graphics exists.
    std::unique_ptr<ps2r::app::DisplayRows> display_rows;
    sequencer.SetStep(BoundarySequencer::Step::MaintDuties, [&](CpuContext& ctx) {
        delivery_meter.DeliverDue(domain.DomainTick());
        StreamLibrary::AdvanceBoundary(domain.DomainTick());
        if (!sound_output.AdvanceBoundary(domain.DomainTick())) return false;
        rpc_router.CompletePending(
            ctx, [](uint32_t addr, CpuContext& c) { DispatchTable::Invoke(addr, c); },
            [](uint32_t a) { return memory::read<uint32_t>(a); });
        const uint64_t now = domain.DomainTick();
        const uint32_t frame = host.PresentKey(
            leg_state->restored_vsync_frame ? *leg_state->restored_vsync_frame : 0u, host.Boundaries());
        ::title::Input::Sample served;
        if (!ps2r::app::SampleInput(input_save, live_input.get(), pad_protocol, pad_stamp, pad_replay, now, frame, served,
                                    pad_probe.get()))
            return false;
        host.OnPadServe(frame, static_cast<uint16_t>(~served.buttons),
            served.lx, served.ly, served.rx, served.ry);
        movie_skip_check(served.buttons, frame);
        fast_forward.NoteButtons(served.buttons);
        // The owned menu extension's value sync (the title's added option pages).
        if (leg_state->menu_extension) leg_state->menu_extension->OnBoundary();
        if (display_rows) display_rows->OnBoundary(leg_state->graphics_application);
        return true;
    });
    // Row 7.1: the mixer's 800-frame output of every boundary is ALSO pushed
    // into the SPSC ring the SDL device callback pulls from. Nothing flows
    // back (the ring's contract), so the audio lane is byte-identical with or
    // without a device. The device opens in play mode only, and only when
    // audio.enabled; audio.volume (0..100) is the gain applied to the ring's
    // copy, never to the hashed buffer.
    ps2r::platform::sdl::AudioPullRing audio_ring;
    // The clock contract's census (rule 5): game_hz / field_hz / render_fps per
    // 1 s wall window; the boundary notes itself in the Verify slot, the presenter
    // notes each frame it posts to the window.
    ps2r::app::ClockCensus clock_census;
    ps2r::platform::sdl::SdlAudioSink audio_sink;
    std::vector<float> audio_gain_buf;
    const float audio_gain = static_cast<float>(std::clamp(cfg.audio.volume, 0, 100)) / 100.0f;
    // The mem lane is the deterministic profile's instrument (diff_legs); in play
    // it cost the game thread ~25 ms every 60th boundary -- a two-field frame once
    // a second (pace_late_trace read 122,182,242,... at ~38 ms of work). Off in
    // play; run.meta mem_lane= names the choice, as gsops_recorder= does.
    host.SetMemLane(!play_mode);
    if (play_mode && cfg.audio.enabled) {
        ps2r::platform::AudioSpec spec;   // 48 kHz stereo, 800 frames per quantum
        // The clock contract, rule 4: the device is wall-clocked; a late game tick is
        // padded with the previous quantum's tail (never a gap, never a pitch change),
        // the lead re-latched by one quantum and a late catch-up burst trimmed back to
        // the standing lead (AudioPullRing::kLeadQuanta, 50 ms), which absorbs an
        // ordinary late tick without any pad.
        audio_ring.SetPadWithTail(true);
        audio_ring.Prime();
        if (!audio_sink.Open(spec, audio_ring))
            PS2R_LOG(HOST, Warn, "audio device not opened: running without sound");
    }
    // SDL stays on process main even when a restored boundary belongs to a
    // different game thread. The interactive presenter owns a separate GL
    // context and receives only the renderer's completed display pixels.
    std::unique_ptr<ps2r::app::SessionRuntime> session;
    if (play_mode && video_live) {
        // The probe is main's to service exactly as the live pad is (its owner).
        session = std::make_unique<ps2r::app::SessionRuntime>(live_input ? live_input.get() : pad_probe.get());
        std::string error;
        const bool recording = host.Armed() && cfg.harness.record_input;
        if (!session->Open(cfg.audio.volume, audio_sink.Running(), recording,
                           recording ? host.InputPath() : std::string(), error, cfg.paths.mc_dir)) {
            std::fprintf(stderr, "session: %s\n", error.c_str());
            return 2;
        }
        session->ConfigurePresentation(cfg.video.vsync, cfg.video.fps_cap);
        session->SetEventTrace(cfg.dev.log_level == ps2r::LogLevelSetting::Narrate);
        session->ConfigureReproductions(ps2r::app::SessionCapture::ReproductionsDirectory(cfg));
        // The player's settings (C4 / track A): seeded from Config, persisted to
        // the settings file in the user data root by the one writer (SettingsFile),
        // never to the memory card. A write failure is loud (stderr + log), never silent.
        session->ConfigureSettings(cfg.ui.perf_overlay,
            [settings = ps2r::app::UserData::SettingsFile(cfg)](const std::string& key, const std::string& value) {
                std::string err;
                if (ps2r::SettingsFile::Set(settings, key, value, &err)) {
                    PS2R_LOG(HOST, Info, "settings: %s=%s -> %s", key.c_str(), value.c_str(), settings.c_str());
                } else {
                    std::fprintf(stderr, "nativeee: settings: %s=%s not saved: %s\n", key.c_str(), value.c_str(), err.c_str());
                    PS2R_LOG(HOST, Warn, "settings: %s=%s not saved: %s", key.c_str(), value.c_str(), err.c_str());
                }
            });
        // --dump-frames <dir> in play: the presented WINDOW image the session ends on
        // (<dir>/w_final.ppm) -- the Root pins on the presented frame read it.
        if (!ps2r::ConfigLoader::Request().dump_frames_dir.empty())
            session->ConfigureWindowDump(ps2r::ConfigLoader::Request().dump_frames_dir);
        // render_fps (the clock contract, rule 5): under video.render_rate=60 the
        // frames the renderer posts; under `display` the frames the presenter
        // shows (each the previous tick's frame re-viewed at the interpolated
        // camera by the video stack's FrameReview, posted from the GS consumer).
        video.SetSessionOutput(
            [&session, &clock_census](std::vector<uint8_t> pixels, int width, int height) {
                if (session->RenderRateSetting() == 0) clock_census.NotePresent();
                session->PostFrame(std::move(pixels), width, height);
            },
            [&session](std::function<void()> operation) { return session->OnMain(std::move(operation)); },
            [&session](std::function<void()> operation) { return session->PostMain(std::move(operation)); });
    }
    // The display shape (video.aspect): `auto` follows the session window's
    // drawable size; without a window (headless, a bootstrap leg) it is 4:3.
    s.aspect = std::make_shared<ps2r::gs::AspectSelection>(ps2r::app::ShapeOf(cfg.video.aspect));
    if (session) session->SetAspect(s.aspect);
    if (session) {
        session->ConfigureDisplayRows({cfg.video.scale, static_cast<int>(cfg.video.aspect),
                                       static_cast<int>(cfg.video.render_rate), static_cast<int>(cfg.video.fullscreen),
                                       cfg.harness.display_hz});
        display_rows = std::make_unique<ps2r::app::DisplayRows>(*session, video,
            s.gs_queue ? s.gs_queue->get() : nullptr, s.consumer ? s.consumer->get() : nullptr,
            host.Armed() && !cfg.harness.out_dir.empty() ? cfg.harness.out_dir + "/camera.trace" : std::string());
        session->SetPresentObserver([&session, &clock_census, rows = display_rows.get()](std::chrono::steady_clock::time_point now) {
            if (session->RenderRateSetting() != 0) clock_census.NotePresentAt(now);
            rows->OnPresent(now);
        });
    }
    // The texture-filtering enhancement (video.texture_filter / video.anisotropy,
    // A2): the GL device samples the owned textures through this selection; the
    // session's Display page changes it live. `title` / 1 (the deterministic
    // profile's pin) leave every sampler at the title's own TEX1 choice.
    auto texture_filter = std::make_shared<ps2r::render::TextureFilterSelection>(
        ps2r::app::FilterOf(cfg.video.texture_filter), cfg.video.anisotropy);
    if (video_live) video.SetTextureFilter(texture_filter);
    if (session) session->SetTextureFilter(texture_filter);
    // Slot 8 (Verify) is set below the park sink: it needs the leg closer for
    // the --frames cap. Its spans and the first-non-silent probe live here.
    const std::vector<ps2r::verify::MemorySpan> verify_spans = {
        {ram.Data(), ram.Size()}, {spad.Data(), spad.Size()}, {arena.Data(), arena.Size()}};
    int64_t audio_first_nonsilent_window = -1;
    ps2r::app::FrameCensus frame_census;
    // Host footprint census (run.meta footprint=): the physical footprint at the
    // 60th boundary (the warmed-up reference), the last boundary and its peak,
    // plus the recorder's retained bytes. A footprint that keeps growing with the
    // frame count names a host container that retains per-frame work.
    FootprintCensus footprint_census;
    ps2r::app::NativeStartup& native_startup = *s.native_startup;
#if PS2R_IOP_ORACLE
    sequencer.SetStep(BoundarySequencer::Step::IopDispatch, [&iop_table, &frame_census, &stream_server, &loader_model, &native_startup](CpuContext&) {
        stream_server.SetBoundary(frame_census.BeginFrame());
        native_startup.OnBoundary();   // the owned transport's per-frame round-trip base
        frame_census.ObserveRpc(stream_server.Calls() + native_startup.RoundTrips());
        frame_census.ObserveCompletions(loader_model.PollDelivered() + loader_model.WaitDelivered() + loader_model.BoundaryDelivered());
        const LoaderFileIo::Census& lc = LoaderFileIo::Stats();
        frame_census.ObservePolls(lc.async_polls_pending + lc.async_polls_done);
        frame_census.ObserveInFrame(iop_table.Kernel().Clock().OffsetInFrameUs());
        iop_table.PumpAtBoundary();
        return true;
    });
    // Slot 6 (EeStreamRpc): the EE services an IOP worker's call into the game's
    // registered EE RPC server (the reply copied back, the Wait::Rpc parkee
    // woken), then the woken parkee runs within the same boundary. The same
    // service also runs from the bridge's poll while the EE itself is inside a
    // RunOnRpcThread (it cannot reach its boundary slot from there).
    EeRpcServer ee_rpc_server(
        SceSif::RegisteredEeServer(), iop_mem, iop_table.Rpc(), [](uint32_t a) { return memory::read<uint8_t>(a); },
        [](uint32_t a, uint8_t v) { memory::write<uint8_t>(a, v); },
        [](uint32_t addr, CpuContext& c) { DispatchTable::Invoke(addr, c); });
    stream_irx.Protocol().SetEeServer(&ee_rpc_server);
#else
    // The play binary: slot 5 is the owned transport's boundary only -- no
    // IOP to pump, no STREAM.IRX calls, the in-frame instant 0 (what the never-
    // pumped kernel clock reported under hle.startup=native).
    sequencer.SetStep(BoundarySequencer::Step::IopDispatch, [&frame_census, &loader_model, &native_startup](CpuContext&) {
        frame_census.BeginFrame();
        native_startup.OnBoundary();   // the owned transport's per-frame round-trip base
        frame_census.ObserveRpc(native_startup.RoundTrips());
        frame_census.ObserveCompletions(loader_model.PollDelivered() + loader_model.WaitDelivered() + loader_model.BoundaryDelivered());
        const LoaderFileIo::Census& lc = LoaderFileIo::Stats();
        frame_census.ObservePolls(lc.async_polls_pending + lc.async_polls_done);
        frame_census.ObserveInFrame(0);
        return true;
    });
#endif
    {
        LoaderFileIo::Deps loader_deps;
        loader_deps.engine = iso_open ? &read_engine : nullptr;
        loader_deps.model = &loader_model;
        loader_deps.now_tick = [&domain] { return domain.DomainTick(); };
        loader_deps.call = [](uint32_t fn, CpuContext& c) { DispatchTable::Invoke(fn, c); };
        // A blocking wait on a record the STREAM module owns yields to the
        // cooperative IOP: the park-loop pump (no frame counted), the EE-side
        // service of a worker's bridged call (a parkee in Wait::Rpc otherwise
        // never wakes), and the RPC end-callbacks -- the work of the boundary's
        // IopDispatch, EeStreamRpc and MaintDuties slots, pulled into the EE's
        // spin as the SIF interrupt does on hardware.
        loader_deps.pump_module = [
#if PS2R_IOP_ORACLE
            &iop_table, &ee_rpc_server,
#endif
            &rpc_router](CpuContext& c) {
#if PS2R_IOP_ORACLE
            iop_table.PumpFromParkLoop();
            ee_rpc_server.Service(c);
            iop_table.PumpAfterEeService();
#endif
            rpc_router.CompletePending(
                c, [](uint32_t addr, CpuContext& cc) { DispatchTable::Invoke(addr, cc); },
                [](uint32_t a) { return memory::read<uint32_t>(a); });
        };
        loader_deps.ring_cache = cfg.loader.ring_cache;
        if (loader_requests.is_open())
            loader_deps.trace = [&loader_requests, &domain](const std::string& line) {
                loader_requests << domain.DomainTick() << ' ' << line << std::endl;   // flushed per line: the leg may end by exit()
            };
        LoaderFileIo::Bind(loader_deps);
    }
#if PS2R_IOP_ORACLE
    iop_table.Rpc().SetEeServicePoll([&ee_rpc_server] {
        if (auto t = ThreadManager::Instance().Current(); t && t->ctx) ee_rpc_server.Service(*t->ctx);
    });
    sequencer.SetStep(BoundarySequencer::Step::EeStreamRpc, [&iop_table, &ee_rpc_server](CpuContext& ctx) {
        ee_rpc_server.Service(ctx);
        iop_table.PumpAfterEeService();
        return true;
    });
#else
    sequencer.SetStep(BoundarySequencer::Step::EeStreamRpc, [](CpuContext&) { return true; });   // no IOP worker can call the EE
#endif
    BoundaryProtocol protocol(tick_source, sequencer.Ticks());
    protocol.SetRunahead(static_cast<uint32_t>(cfg.engine.ee_runahead));
    s.live_input = &live_input;
    s.pad_probe = &pad_probe;
    s.input_save_selection = &input_save_selection;
    s.input_save = &input_save;
    s.audio_ring = &audio_ring;
    s.clock_census = &clock_census;
    s.audio_sink = &audio_sink;
    s.audio_gain_buf = &audio_gain_buf;
    s.audio_gain = &audio_gain;
    s.session = &session;
    s.display_rows = &display_rows;
    s.verify_spans = &verify_spans;
    s.audio_first_nonsilent_window = &audio_first_nonsilent_window;
    s.frame_census = &frame_census;
    s.footprint_census = &footprint_census;
#if PS2R_IOP_ORACLE
    s.ee_rpc_server = &ee_rpc_server;
#endif
    s.protocol = &protocol;
    return ArmKernelClocks(s);
}

int EeBootstrap::ArmKernelClocks(LegState& s) {
    const bool& play_mode = *s.play_mode;
    Intc& intc = *s.intc;
    EmuDomain& domain = *s.domain;
    DerivedClocks& clocks = *s.clocks;
    std::atomic<bool>& running = *s.running;
    ps2r::gs::GsState& gs_state = *s.gs_state;
    std::unique_ptr<ps2r::gs::DrawIrQueueConsumer>& consumer = *s.consumer;
    ps2r::gs::CountingDrawIrSink& draw_sink = *s.draw_sink;
    Dmac& dmac = *s.dmac;
    std::unique_ptr<MtvuStack>& mtvu = *s.mtvu;
    uint64_t& dmac_tail_chases = *s.dmac_tail_chases;
    uint64_t& dmac_tail_chases_parked = *s.dmac_tail_chases_parked;
    uint64_t& gs_syncpath_flushes = *s.gs_syncpath_flushes;
    uint64_t& gs_syncpath_parked = *s.gs_syncpath_parked;
    ps2r::sound::Mixer& mixer = *s.mixer;
    BoundarySequencer& sequencer = *s.sequencer;
    uint64_t& gs_frame_boundaries = *s.gs_frame_boundaries;
    LoaderModel& loader_model = *s.loader_model;
    ps2r::platform::sdl::AudioPullRing& audio_ring = *s.audio_ring;
    std::vector<float>& audio_gain_buf = *s.audio_gain_buf;
    const float& audio_gain = *s.audio_gain;
    std::unique_ptr<ps2r::app::SessionRuntime>& session = *s.session;
    BoundaryProtocol& protocol = *s.protocol;
    // The kernel's clocks: TimerWaitMicroseconds counts T0 in the tick domain
    // and yields through the boundary protocol's wait (the wait advances the
    // emu clock at the boundary, never sleeps); the alarm tick clock is the
    // domain tick. Without this the first timer wait parks tick_clock_absent.
    // A display tick: one VSync job through the sink chain with arg1 = 2 (no EE
    // frame mark; the frames lane, the present policy and `frames=` are the
    // Verify slot's and untouched) -- the pipeline grabs the scanout and the
    // presenter shows it. Play mode only: deterministic legs never call it (the
    // inert pin). Callers: the FMV pace park (one per field waited through), a
    // park-less late serve, and the play-mode timer wait below.
    uint64_t display_ticks = 0, display_ticks_late = 0;   // late = presented at a serve no park followed
    // The device push of one pumped vsync quantum: audio.volume (0..100) is the
    // gain applied to the ring's copy, never to the hashed buffer.
    uint64_t audio_device_quanta = 0, audio_device_nonsilent = 0;   // census: quanta pushed / with a non-zero sample
    auto push_audio_ring = [&audio_ring, &audio_gain, &audio_gain_buf, &audio_device_quanta, &audio_device_nonsilent,
                           &session](const float* out) {
        const float gain = session ? static_cast<float>(session->Volume()) / 100.0f : audio_gain;
        ++audio_device_quanta;
        for (int i = 0; i < ps2r::sound::Mixer::kFramesPerVsync * 2; ++i)
            if (out[i] != 0.0f) { ++audio_device_nonsilent; break; }
        if (gain == 1.0f) {
            audio_ring.Push(out, ps2r::sound::Mixer::kFramesPerVsync);
        } else {
            audio_gain_buf.assign(out, out + ps2r::sound::Mixer::kFramesPerVsync * 2);
            for (float& v : audio_gain_buf) v *= gain;
            audio_ring.Push(audio_gain_buf.data(), ps2r::sound::Mixer::kFramesPerVsync);
        }
    };
    // The play-mode FIELD PUMP (the SPU2 half of the display tick): the SPU2
    // mixes 48000 frames a second whether or not the EE's frame loop runs -- a
    // movie's PCM (the core-0 external input at BVOLL/BVOLR, which the title
    // programs to 0x7FFF at movie start and back to 0 at its end) and the keyed
    // voices of a load screen play while the EE never passes a boundary. The
    // boundary pump alone would mix the movie's queued PCM only AFTER the
    // title zeroed BVOL (silence) and would leave the device starving for
    // seconds. So the mixer is pumped ONE vsync quantum per DOMAIN TICK (one
    // field): the boundary's own pump (the Verify slot, hashed) covers the
    // boundary's tick, and every display tick -- the FMV pace park's parked
    // fields, a late serve, the kernel timer wait, the disc wait -- catches the
    // mixer up to the fields the domain advanced meanwhile, each quantum pushed
    // to the DEVICE ring only. The hash lane keeps its shape: the Verify slot
    // still pumps and hashes exactly 800 frames per EE boundary. Play mode only
    // -- deterministic legs never reach a field pump (the inert pin). The
    // one-line toggle: `audio_field_pump_on` (false = the boundary-only pump of
    // the original).
    const bool audio_field_pump_on = play_mode;
    uint64_t snd_pumps_fields = 0;
    uint64_t audio_pumped_tick = 0;   // the domain tick the mixer is accounted up to
    // `reserve` = ticks the caller mixes itself afterwards (the boundary's hashed pump).
    auto audio_field_pump = [&](uint64_t reserve) {
        if (!audio_field_pump_on) return;
        const uint64_t now = domain.DomainTick();
        while (audio_pumped_tick + reserve < now) {
            mixer.Pump();
            push_audio_ring(mixer.Output());
            ++audio_pumped_tick;
            ++snd_pumps_fields;
        }
    };
    auto display_tick = [&](bool late) {
        audio_field_pump(0);   // one SPU2 field per display field the domain advanced
        // A5: the movie-skip press, read at the parked fields (the boundary's pad
        // serve does not run while the EE is parked in a movie).
        if (play_mode && s.movie_skip_poll) (*s.movie_skip_poll)();
        if (!play_mode || !consumer) return;
        ps2r::gs::JobRec vs{};
        vs.type = static_cast<uint32_t>(ps2r::gs::JobKind::VSync);
        vs.seq = gs_state.Seq();
        vs.src_frame = static_cast<uint32_t>(gs_frame_boundaries);
        vs.arg1 = 2;   // a display tick, not an EE-pushed game-frame boundary
        vs.vram_epoch = gs_state.Vram().UploadEpoch();
        draw_sink.OnFrame(vs);
        ++display_ticks;
        if (late) ++display_ticks_late;
    };
    // Play mode: a guest timer wait (the kernel's TimerWaitMicroseconds busy-wait
    // on T0 -- a load wait parks here under the pacer) presents one display tick
    // per field it waited through, exactly as the FMV pace park does, so the
    // window keeps showing the scanout while the title spins. The field count is
    // the domain's (never the wall); the wait itself is the protocol's unchanged.
    // Deterministic legs install the protocol's hook as it is.
    uint64_t wait_display_ticks = 0, wait_hook_calls = 0;
    std::function<void(uint64_t)> wait_hook = protocol.WaitHook();
    if (play_mode) {
        wait_hook = [&domain, &display_tick, &wait_display_ticks, &wait_hook_calls, inner = protocol.WaitHook()](uint64_t deadline) {
            ++wait_hook_calls;
            const uint64_t before = domain.DomainTick();
            inner(deadline);
            if (domain.DomainTick() != before) {
                display_tick(false);
                ++wait_display_ticks;
            }
        };
    }
    clocks.InstallKernelClocks(SyscallTable::Instance(), wait_hook);
    // cdvd.delivery_visibility=poll: a BLOCKING disc wait whose modelled drive end
    // lies beyond the current frame's edge parks the EE for whole frames (the
    // console's EE is blocked in the sync wait while vblanks pass). One park = one
    // field of emulated time through the boundary protocol's wait -- the same
    // mechanism as the FMV pace park and the kernel timer wait: T0 advances one
    // field, the vblank is raised, no sub-tick -- repeated until the domain tick
    // moved (a demanded field the quantum absorbs advances nothing; a demand from
    // a non-guest thread is deferred to the next boundary and ends the park).
    // Play mode presents one display tick per parked field, as the FMV park does.
    // Boundary mode never calls this (LoaderModel::CompleteNow parks under poll only).
    uint64_t cdvd_park_waits = 0;   // protocol waits the disc parks issued (absorbed ones included)
    const auto park_file_frame = [&domain, &protocol, &running, &display_tick, &cdvd_park_waits] {
        const uint64_t target = domain.DomainTick() + 1;
        while (domain.DomainTick() < target && running.load(std::memory_order_relaxed)) {
            const uint64_t deferred = protocol.DeferredDemands();
            protocol.Wait(0);
            ++cdvd_park_waits;
            if (protocol.DeferredDemands() != deferred) break;   // not the guest thread: fires at the next boundary
        }
        if (domain.DomainTick() >= target) display_tick(false);
    };
    loader_model.SetParkFrame(park_file_frame);
    StreamLibrary::SetParkFrame(park_file_frame);
    GsSync::Bind(&sequencer, &clocks);
    // The frame boundary's DMAC half (OLD ran it inside sceGsSyncV / WaitForVblank
    // before the VSync push): both boundary sites are GsSync::SyncV, which runs
    // the chase BEFORE RunBoundary -- so before slot 0 (PresentRendezvous), the
    // field tick (TickAdvance) and the present. The fixed Step enum has no slot
    // ahead of PresentRendezvous and InstallPhase3Steps owns that slot's body,
    // so the pre-sequence hook is the one place that keeps the order without
    // re-ordering the enum. sceGsSyncPath flushes the parked kick before it
    // reports the path idle.
    // Under MTVU the chase is followed by the boundary ladder (feed complete ->
    // deferred boundary slot + one drain pass while the worker runs, else the
    // terminal Sync(SceGsSyncV)), and sceGsSyncPath's flush is preceded by the
    // site's DeferOn wrapper; with the stack absent both are exactly the
    // landed chase / flush.
    GsSync::BindDmac(
        [&dmac, &mtvu, &dmac_tail_chases, &dmac_tail_chases_parked] {
            if (dmac.HasDeferredVif1()) ++dmac_tail_chases_parked;
            dmac.ChaseVif1Tail();
            ++dmac_tail_chases;
            if (mtvu) mtvu->FrameSync(GsSync::SyncVCount(), ps2r::platform::Current().now_ns());
        },
        [&dmac, &mtvu, &gs_syncpath_flushes, &gs_syncpath_parked] {
            if (mtvu) mtvu->SiteSync(SyncSite::SceGsSyncPath);
            if (dmac.HasDeferredVif1()) ++gs_syncpath_parked;
            dmac.FlushDeferredVif1();
            ++gs_syncpath_flushes;
        });

    // --- kernel seams ---------------------------------------------------------
    auto guest_call = [](uint32_t addr, CpuContext& ctx) {
        DispatchTable::Invoke(addr, ctx);
        return true;
    };
    SyscallTable::Instance().SetGuestCall(guest_call);
    dmac.SetGuestCall([](uint32_t addr, CpuContext& ctx) {
        DispatchTable::Invoke(addr, ctx);
        return true;
    });
    // The kernel's DMAC handler records (AddDmacHandler, also after boot) reach
    // the DMAC's registry lazily: installed field-for-field before every
    // dispatch, so a handler added while running is seen by the next one. The
    // registry's next id is the kernel's counter (max id + 1; ids never reuse).
    auto install_dmac_handlers = [&dmac] {
        std::vector<DmacHandler> records;
        int next_id = 1;
        for (const SyscallTable::KernelHandler& k : SyscallTable::Instance().DmacHandlers()) {
            DmacHandler h;
            h.id = k.id;
            h.cause = k.cause;
            h.pc = k.pc;
            h.gp = k.gp;
            h.arg = k.arg;
            h.flag = k.flag;
            h.active = k.active;
            records.push_back(h);
            if (k.id >= next_id) next_id = k.id + 1;
        }
        dmac.handlers().Install(records, next_id);
    };
    // sceSifSetDma's completion rule: the kernel's SIF1 DMA raises D_STAT.CIS(6)
    // and runs the DMAC handlers when CIS & CIM is non-zero. The mask itself is
    // never mirrored from the syscall table into D_STAT (it changes only by a
    // savestate restore), so this dispatches nothing on a cold boot.
    SyscallTable::Instance().SetSifDmaHook([&dmac, install_dmac_handlers](CpuContext& ctx) {
        dmac.globals().stat |= DmacStat::Cis(6);
        if (dmac.CheckInterrupt()) {
            install_dmac_handlers();
            dmac.DispatchInterrupt(ctx);
        }
    });
    std::string first_syscalls;
    uint64_t syscall_count = 0;
    SyscallTable::Instance().SetTrace([&](int num, const char* name) {
        constexpr uint64_t kKeep = 40;
        if (syscall_count++ < kKeep)
            first_syscalls += (first_syscalls.empty() ? "" : ",") + std::string(name ? name : "?") + "(" +
                              std::to_string(num) + ")";
    });
    SyscallTable::Instance().BindIntcMask(intc.MaskWord());
    intc.SetGuestCall(guest_call);
    ThreadManager::Instance().InstallEventApplier();
    ThreadManager::Instance().SetGuestBody([](int, CpuContext& ctx) { DispatchTable::Invoke(ctx.cpuRegs.pc, ctx); });

    s.display_ticks = &display_ticks;
    s.display_ticks_late = &display_ticks_late;
    s.audio_device_quanta = &audio_device_quanta;
    s.audio_device_nonsilent = &audio_device_nonsilent;
    std::function<void(const float*)> push_audio_ring_fn = push_audio_ring;
    s.push_audio_ring = &push_audio_ring_fn;
    s.audio_field_pump_on = &audio_field_pump_on;
    s.snd_pumps_fields = &snd_pumps_fields;
    s.audio_pumped_tick = &audio_pumped_tick;
    std::function<void(uint64_t)> audio_field_pump_fn = audio_field_pump;
    s.audio_field_pump = &audio_field_pump_fn;
    std::function<void(bool)> display_tick_fn = display_tick;
    s.display_tick = &display_tick_fn;
    s.wait_display_ticks = &wait_display_ticks;
    s.wait_hook_calls = &wait_hook_calls;
    s.cdvd_park_waits = &cdvd_park_waits;
    s.first_syscalls = &first_syscalls;
    s.syscall_count = &syscall_count;
    return LoadImageAndMovie(s);
}
