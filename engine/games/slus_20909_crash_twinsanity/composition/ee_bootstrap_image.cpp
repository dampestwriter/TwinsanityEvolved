// Phase functions of the composition root (EeBootstrap::Run split, ARCHITECTURE section 11):
// each phase unpacks the upstream objects it uses from LegState, constructs its own, registers
// the ones later phases need and tail-calls the next phase, so object lifetimes nest exactly as
// the former single scope did (later phases' objects are destroyed first).
#include "games/slus_20909_crash_twinsanity/composition/leg_state.h"
#include "engine/ee/timing/tick_profile.h"

#include <cstdio>
#include <deque>

#include "engine/ee/cpu/shadow_call_stack.h"

// The generated common TU's shadow call stack storage (recompiled.cpp: thread_local,
// external linkage). The game thread takes the address of ITS instance for the tick
// census sampler (TickProfile::SetStackView); nothing else touches it here.
struct EeCallFrame { uint32_t func_addr; uint32_t ra_at_entry; };
static constexpr size_t EE_CALL_STACK_MAX = 128;
extern thread_local EeCallFrame g_ee_call_stack[EE_CALL_STACK_MAX];
extern thread_local size_t g_ee_call_stack_depth;
#include "games/slus_20909_crash_twinsanity/hle/audio/music_buffers.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/music_library.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_effects.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_transport.h"

int EeBootstrap::LoadImageAndMovie(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    const TitleProfile& title = *s.title;
    const EeBootstrap::Paths& paths = *s.paths;
    const bool& play_mode = *s.play_mode;
    MainMemory& ram = *s.ram;
    Scratchpad& spad = *s.spad;
    AddressMap& map = *s.map;
    EmuDomain& domain = *s.domain;
    EeTimers& timers = *s.timers;
    std::atomic<bool>& running = *s.running;
    Dmac& dmac = *s.dmac;
    std::unique_ptr<MtvuStack>& mtvu = *s.mtvu;
#if PS2R_IOP_ORACLE
    ps2r::ipu::DmacChannelFaceAdapter& ipu_dmac_face = *s.ipu_dmac_face;
    ps2r::ipu::LibipuApi& libipu = *s.libipu;
#endif
    IsoFs& iso_fs = *s.iso_fs;
    StreamReader& stream_reader = *s.stream_reader;
    ps2r::sound::SpuCore& spu_core = *s.spu_core;
    ps2r::app::SoundOutput& sound_output = *s.sound_output;
    BoundaryProtocol& protocol = *s.protocol;
    std::function<void(bool)>& display_tick = *s.display_tick;
    // --- ELF image + title data map + post-load patches + boot env -------------
    {
        std::vector<uint8_t> file;
        ElfLoader::Image image;
        std::string err;
        if (!ElfLoader::ReadFile(paths.elf, file, &err) || !ElfLoader::Load(file, ram, image, &err)) {
            std::fprintf(stderr, "nativeee: ELF %s: %s\n", paths.elf.c_str(), err.c_str());
            return 2;
        }
        if (image.entry != title.entry) {
            std::fprintf(stderr, "nativeee: ELF entry 0x%08x != profile entry 0x%08x\n", image.entry, title.entry);
            return 2;
        }
        DataMapApplier::Result res;
        // The RAM data map is a proven SUBSET of the ELF image just loaded
        // (19,278 entries covered by the ELF with 0 mismatches,
        // 3 zero entries in .bss), so applying it changes no
        // byte; the source tree keeps it for the ElfLoaderSlus20909 probe and a
        // development build still applies it, while the shipped artifact carries
        // no copy of the title's data words and skips it. The
        // post-load patches (one word differs from retail: P3-RAMIMAGE-2) are
        // required in both.
        {
            const std::string map = paths.title_dir + "/data/ram_data_map_v50.txt";
            if (std::FILE* probe = std::fopen(map.c_str(), "rb")) {
                std::fclose(probe);
                if (!DataMapApplier::ApplyFile(map, ram, res, &err)) {
                    std::fprintf(stderr, "nativeee: /data/ram_data_map_v50.txt: %s\n", err.c_str());
                    return 2;
                }
            } else {
                PS2R_LOG(HOST, Info, "data map absent (artifact layout): the ELF image is the RAM image");
            }
        }
        if (!DataMapApplier::ApplyFile(paths.title_dir + "/data/post_load_patches.txt", ram, res, &err)) {
            std::fprintf(stderr, "nativeee: /data/post_load_patches.txt: %s\n", err.c_str());
            return 2;
        }
        PS2R_LOG(HOST, Info, "ELF loaded: entry=0x%08x segments=%zu data-map entries=%zu", image.entry,
                 image.segments.size(), res.entries);
    }
    BootEnv::Instance().Configure(title.boot);
    BootEnv::Instance().PreSeed();

    // The libdma rows' instance (thunks over it run for the whole leg). The
    // libgraph rows store through the GS page (the router routes it to gs_state)
    // AND record into the same GsPrivRegs -- gs_state.Priv() is the ONE shadow.
    LibdmaBindings libdma(dmac, ram, spad);
    // The libdma sync rows' MTVU barrier (sites SceDmaSync / SceGifSyncDma)
    // runs before each row's flush; absent the stack the rows are the flush.
    if (mtvu) {
        libdma.SetSyncHook([](void* user, bool gif_sync) {
            static_cast<MtvuStack*>(user)->SiteSync(gif_sync ? SyncSite::SceGifSyncDma : SyncSite::SceDmaSync);
        }, mtvu.get());
    }
    // Movie callbacks and MPEG read-ahead share one cursor. PCM enters the
    // existing mixer after voices, gated by the first picture; source Close
    // flushes it. The audio owner keeps the remaining common mixer/gain handoff.
    ps2r::ipu::GuestMemoryAdapter guest_mem([&map](uint32_t addr, size_t len) { return map.Translate(addr, len); });
    title::MovieStream movie_stream(guest_mem,
        [&iso_fs](uint64_t offset, size_t bytes, uint8_t* destination) -> int64_t {
            return iso_fs.IsOpen() ? static_cast<int64_t>(iso_fs.ReadBytes(offset, bytes, destination)) : -1;
        });
    const auto read_movie_sectors = [&](uint8_t* destination, uint32_t sectors) -> int32_t {
        if (movie_stream.OwnsCursor()) return movie_stream.Read(destination, sectors);
        uint32_t error = 0;
        const uint32_t delivered = stream_reader.Read(sectors, destination, &error);
        return error ? -1 : static_cast<int32_t>(delivered);
    };
    const ps2r::ipu::FmvContextLayout fmv_layout = FmvContextLayoutData::CrashTwinsanityUS();
    ps2r::ipu::FmvSectorSourceAdapter fmv_sectors([&](uint32_t sectors, uint8_t* dst) {
        const int32_t result = read_movie_sectors(dst, sectors);
        return result < 0 ? 0u : static_cast<uint32_t>(result);
    });
    ps2r::sound::FmvPcmInput fmv_pcm;
    // One Movie PCM owner (MovieSound/MovieAudio: gain + FIFO lifetime, MOVI)
    // on exactly one sink seat: SoundOutput routes the owner to the owned
    // output when hle.audio_mix is selected, otherwise to the common Mixer's
    // PcmSource seat. No second FIFO consumer and no second gain record.
    ps2r::app::MovieSound movie_sound(cfg.hle.movie_lifecycle, fmv_pcm, spu_core,
        ps2r::app::MovieSound::Sink{[&sound_output](ps2r::sound::PcmSource* source) { sound_output.BindMovie(source); }});
    const ps2r::ipu::IpuSettings ipu_settings;
    ps2r::ipu::MpegApi mpeg(guest_mem, fmv_layout, fmv_sectors, fmv_pcm,
                            [] { return std::unique_ptr<ps2r::ipu::VideoDecoder>(new ps2r::ipu::FfmpegVideoDecoder()); },
                            ipu_settings);
    // Pump pending timers before serving; book pacing on the existing domain
    // clock and wait through its non-consuming protocol. In play mode each
    // elapsed field, including an unparked serve, uses display_tick (VSync arg1
    // = 2, no additional game-frame mark). Deterministic legs keep Verify's
    // existing presentation/frame accounting; they never call display_tick.
    // Play mode: the host may fall behind real time (a movie picture costs more
    // than its display period); the pacer then re-anchors after 4 periods of
    // debt so the movie runs in slow motion rather than draining hundreds of
    // pictures as a blur. Off (the reference contract) in deterministic legs.
    if (play_mode) mpeg.SetPacerReanchorPeriods(4);
    // (display_tick is defined above InstallKernelClocks: the play-mode timer
    // wait presents through it too.)
    ps2r::ipu::LibmpegBindings::Hooks mpeg_hooks;
    mpeg_hooks.pump_timers = [&timers](CpuContext& c) { timers.PumpAtBoundary(c); };
    mpeg_hooks.now_tick = [&domain] { return static_cast<int64_t>(domain.DomainTick()); };
    mpeg_hooks.park = [&domain, &protocol, &running, &display_tick](int64_t ticks) {
        const uint64_t target = domain.DomainTick() + static_cast<uint64_t>(ticks);
        uint64_t shown_at = domain.DomainTick();
        while (domain.DomainTick() < target && running.load(std::memory_order_relaxed)) {
            protocol.Wait(0);
            const uint64_t now = domain.DomainTick();
            if (now != shown_at) {   // one display present per field the park waited through
                shown_at = now;
                display_tick(false);
            }
        }
    };
    mpeg_hooks.on_served = [&display_tick](bool no_park) {
        if (no_park) display_tick(true);   // no park follows: present the previous picture now
    };
#if PS2R_IOP_ORACLE
    ps2r::ipu::LibmpegBindings libmpeg(mpeg, guest_mem, fmv_layout, mpeg_hooks, cfg.hle.movie_lifecycle ?
        ps2r::ipu::LibmpegBindings::RetainedInit{} :
        ps2r::ipu::LibmpegBindings::RetainedInitialization(libipu, ipu_dmac_face));
    ps2r::ipu::LibipuBindings libipu_bind(libipu, guest_mem);
#else
    // The play binary (ledger L7): hle.movie_lifecycle is on (ConfigLoader refuses
    // the retained arm before this point), so the retained sceMpegInit
    // initialisation and the libipu rows are not linked; the four libipu.a title
    // rows install as trap rows (sdk_rows_trap), never reached on the owned route.
    ps2r::ipu::LibmpegBindings libmpeg(mpeg, guest_mem, fmv_layout, mpeg_hooks, ps2r::ipu::LibmpegBindings::RetainedInit{});
#endif

    s.libdma = &libdma;
    s.guest_mem = &guest_mem;
    s.movie_stream = &movie_stream;
    std::function<int32_t(uint8_t*, uint32_t)> read_movie_sectors_fn = read_movie_sectors;
    s.read_movie_sectors = &read_movie_sectors_fn;
    s.fmv_layout = &fmv_layout;
    s.fmv_sectors = &fmv_sectors;
    s.fmv_pcm = &fmv_pcm;
    s.movie_sound = &movie_sound;
    s.mpeg = &mpeg;
    s.mpeg_hooks = &mpeg_hooks;
    s.libmpeg = &libmpeg;
#if PS2R_IOP_ORACLE
    s.libipu_bind = &libipu_bind;
#endif
    return InstallDispatch(s);
}

namespace {
// The libipu.a rows (ledger L7): the oracle binary binds LibipuBindings over its IPU
// model; the play binary binds nothing -- the four title rows install as trap rows
// (sdk_rows_trap), never reached on the owned movie route (libipu_calls all 0).
int BindLibipuRows(SdkBindingManifest& manifest, ps2r::ipu::LibipuBindings* bind) {
#if PS2R_IOP_ORACLE
    return ps2r::ipu::LibipuBindings::Bind(manifest, bind);
#else
    (void)manifest; (void)bind;
    return 0;
#endif
}

// The tick census (diag.tick_census): every bound game thread publishes ITS
// shadow call stack storage (the generated TU's thread_local frames and
// depth) for the census sampler to read; the sampler thread starts here.
// Called on the game thread (InstallDispatch runs on it).
void InstallTickCensus(bool census_on) {
    ps2r::ee::TickProfile::SetStackViewProvider([](ps2r::ee::TickProfile::StackView* out) {
        out->func_addrs = reinterpret_cast<const uint32_t*>(&g_ee_call_stack[0].func_addr);
        out->stride_u32 = sizeof(EeCallFrame) / sizeof(uint32_t);
        out->depth = &g_ee_call_stack_depth;
        out->capacity = EE_CALL_STACK_MAX;
        return true;
    });
    ps2r::ee::TickProfile::BindGameThread();
    ps2r::ee::TickProfile::EnableCensus(census_on);
    ps2r::ee::TickProfile::SetFrameReader([](uint32_t n, uint32_t* func_addr, uint32_t* ra) {
        ShadowCallStack::Frame f;
        if (!ShadowCallStack::Top(n, f)) return false;
        if (func_addr) *func_addr = f.func_addr;
        if (ra) *ra = f.ra;
        return true;
    });
}
}  // namespace

int EeBootstrap::InstallDispatch(LegState& s) {
    const ps2r::Config& cfg = *s.cfg;
    const bool& play_mode = *s.play_mode;
    std::unique_ptr<ps2r::app::SessionRuntime>& session = *s.session;
    ps2r::verify::VerifyHost& host = *s.host;
    const TitleProfile& title = *s.title;
    const EeBootstrap::Paths& paths = *s.paths;
    MainMemory& ram = *s.ram;
    MetaArena& arena = *s.arena;
    EmuDomain& domain = *s.domain;
    DerivedClocks& clocks = *s.clocks;
    SifState& sif_state = *s.sif_state;
    ps2r::gs::GsState& gs_state = *s.gs_state;
    ps2r::app::VideoStack& video = *s.video;
    ps2r::app::GraphicsOutput& graphics_output = *s.graphics_output;
    ps2r::app::EffectsApplication& effects_application = *s.effects_application;
    std::unique_ptr<MtvuStack>& mtvu = *s.mtvu;
#if PS2R_IOP_ORACLE
    IopModuleTable& iop_table = *s.iop_table;
    ps2r::title::stream_irx::StreamIrxPlugin& stream_irx = *s.stream_irx;
    EeRpcServer& ee_rpc_server = *s.ee_rpc_server;
#endif
    IsoFs& iso_fs = *s.iso_fs;
    const bool& iso_open = *s.iso_open;
    ReadEngine& read_engine = *s.read_engine;
    StreamReader& stream_reader = *s.stream_reader;
    ps2r::sound::SpuCore& spu_core = *s.spu_core;
    ps2r::app::SoundOutput& sound_output = *s.sound_output;
    DeliveryMeter& delivery_meter = *s.delivery_meter;
    LoaderModel& loader_model = *s.loader_model;
    ps2r::app::ServiceTimeline& service_timeline = *s.service_timeline;
    RpcRouter& rpc_router = *s.rpc_router;
    McHostStore& mc_store = *s.mc_store;
    McCommandModel& mc_model = *s.mc_model;
    PadProtocol& pad_protocol = *s.pad_protocol;
    std::function<uint64_t()>& pad_now = *s.pad_now;
    ps2r::sound::hle::SdrRemote& sdr_remote = *s.sdr_remote;
    ps2r::app::InputSave& input_save = *s.input_save;
    ps2r::app::FrameCensus& frame_census = *s.frame_census;
    LibdmaBindings& libdma = *s.libdma;
    ps2r::ipu::GuestMemoryAdapter& guest_mem = *s.guest_mem;
    title::MovieStream& movie_stream = *s.movie_stream;
    std::function<int32_t(uint8_t*, uint32_t)>& read_movie_sectors = *s.read_movie_sectors;
    const ps2r::ipu::FmvContextLayout& fmv_layout = *s.fmv_layout;
    ps2r::app::MovieSound& movie_sound = *s.movie_sound;
    ps2r::app::NativeStartup& native_startup = *s.native_startup;
    ps2r::ipu::MpegApi& mpeg = *s.mpeg;
    ps2r::ipu::LibmpegBindings& libmpeg = *s.libmpeg;
    // --- generated dispatch + SDK binding manifest + title workarounds ---------
    // This thread runs recompiled EE code too (the boot path and the boundary
    // duties' guest calls): it takes the EE FPU's round-toward-zero mode like
    // every guest worker thread (ThreadManager::RunGuestThread).
    Ps2Float::SetGuestThreadRounding();
    InstallTickCensus(cfg.diag.tick_census);   // this thread's hook work is the tick's (TickProfile)
    DispatchTable::Initialize();
    {
        SdkBindingManifest& manifest = SdkBindingManifest::Instance();
        manifest.RegisterBuiltins();
        GsSync::Register(manifest);
        // Phase 4C: the sceSif bodies displace KernlSifRpc's park rows (Rebind;
        // the park owner is not edited). The IOP heap is the module's sysmem
        // arena; a restored anchor's client handle (0xDEAD0000|sid) decodes to
        // the title's server id here (the engine knows no server id).
        SceSif::IopHeap iop_heap;
        if (native_startup.Selected()) {
            // No IOP, no heap: every allocation is refused (0) and counted (iop_heap_native=).
            iop_heap.alloc = [&native_startup](uint32_t size) { return native_startup.AllocIopHeap(size); };
            iop_heap.free = [&native_startup](uint32_t addr) { native_startup.FreeIopHeap(addr); };
            SceSif::SetModuleLoader([&native_startup](const std::string& path) { return native_startup.LoadModule(path); });
        }
#if PS2R_IOP_ORACLE
        else {
            iop_heap.alloc = [&stream_irx](uint32_t size) { return stream_irx.Sysmem().Heap().Alloc(size); };
            iop_heap.free = [&stream_irx](uint32_t addr) { stream_irx.Sysmem().Heap().Free(addr); };
            SceSif::SetModuleLoader({});
        }
#endif
        const int sif_rows = SceSif::Bind(manifest, rpc_router, sif_state, std::move(iop_heap), [](uint32_t handle) {
            if ((handle & 0xFFFF0000u) != 0xDEAD0000u) return 0u;
            const uint32_t low = handle & 0xFFFFu;
            if (low == (ps2r::app::kStreamRpcServerId & 0xFFFFu)) return ps2r::app::kStreamRpcServerId;
            return low ? (0x80000000u | low) : 0u;   // the SDK system servers keep their low half
        });
        if (auto* meta = host.Meta()) meta->Set("sif_rows", std::to_string(sif_rows));
        // The EE RPC server thread (0x1e7f80) sleeps inside sceSifRpcLoop
        // (SceSif::RpcLoop = SleepThread). As an opaque body that thread could
        // never be captured ("logical stack may contain omitted frames"); as a
        // declared wait body it is restartable like the generated SleepThread
        // wrapper (the relauncher re-enters it and it sleeps again).
        if (!manifest.MarkWaitBody("libkernl.a", "sceSifRpcLoop", TSW_SLEEP)) return 2;
        SceSif::SetInitRpcStatics(SifInitStatics::CrashTwinsanityUS());   // libkernl's sceSifInitRpc statics (title data)
        const int cdvd_rows = LibcdvdBindings::Bind(manifest, iso_open ? &read_engine : nullptr, &clocks, &stream_reader);
        // `hle.libmc_format_in_t0`: where sceMcGetInfo reads its fifth argument.
        LibmcBindings::SetFormatInT0(cfg.hle.libmc_format_in_t0);
        const int libmc_rows = LibmcBindings::Bind(manifest, &mc_model, &mc_store);
        LibpadBindings::Statics pad_statics;
        pad_statics.port_table = title.pad.port_table;
        pad_statics.open_flags = title.pad.open_flags;
        pad_statics.static_area = title.pad.static_area;
        const int libpad_rows = LibpadBindings::Bind(manifest, &pad_protocol, pad_statics, pad_now);
        const int libsdr_rows = ps2r::sound::hle::LibsdrBindings::Bind(manifest, &sdr_remote);
        const int libdma_rows = LibdmaBindings::Bind(manifest, &libdma);
        ps2r::gs::LibgraphBindings::Statics gs_statics;
        gs_statics.gparam_addr = title.gs.gparam_addr;
        const int libgraph_rows = ps2r::gs::LibgraphBindings::Bind(manifest, &gs_state.Priv(), gs_statics);
        const int libmpeg_rows = ps2r::ipu::LibmpegBindings::Bind(manifest, &libmpeg);
        const int libipu_rows = BindLibipuRows(manifest, s.libipu_bind);
        if (auto* meta = host.Meta()) {
            meta->Set("cdvd_rows", std::to_string(cdvd_rows));
            meta->Set("libmc_rows", std::to_string(libmc_rows));
            meta->Set("libmc_format_in_t0", cfg.hle.libmc_format_in_t0 ? "on" : "off");
            meta->Set("libpad_rows", std::to_string(libpad_rows));
            meta->Set("libsdr_rows", std::to_string(libsdr_rows));
            meta->Set("libdma_rows", std::to_string(libdma_rows));
            meta->Set("libgraph_rows", std::to_string(libgraph_rows));
            meta->Set("libmpeg_rows", std::to_string(libmpeg_rows));
            meta->Set("libipu_rows", std::to_string(libipu_rows));
            meta->Set("avcodec_version", ps2r::ipu::FfmpegVideoDecoder::LibraryVersion());
            meta->Set("iso_open", iso_open ? "1" : "0");
            meta->Set("cdvd_latency_model", cfg.cdvd.latency_model == ps2r::CdvdLatencyModel::Modelled ? "modelled" : "instant");
        }
        std::vector<SdkBindingManifest::TitleRow> rows;
        std::string err;
        const std::string tsv = paths.title_dir + "/" + title.sdk_bindings_file;
        if (!SdkBindingManifest::LoadTitleManifest(tsv, rows, &err)) {
            std::fprintf(stderr, "nativeee: manifest %s: %s\n", tsv.c_str(), err.c_str());
            return 2;
        }
        int traps = 0, unresolved = 0;
        const std::vector<SdkBindingManifest::Resolved> resolved = manifest.Resolve(rows);
        // Wait bodies (sceSifRpcLoop): a guest frame (fn, $ra) around the park
        // and the continuation proof (entry = start, restart_wait) the thread
        // capture/relaunch contract needs; stable entry storage for the proof.
        static std::deque<uint32_t> wait_entries;
        const int installed = manifest.Install(
            resolved, [](uint32_t a, SdkBindingManifest::Body b) { return DispatchTable::Install(a, b); },
            [](uint32_t a) { return DispatchTable::Capture(a); }, &traps, &unresolved,
            [](uint32_t addr, SdkBindingManifest::Body body, int wait_type) {
                wait_entries.push_back(addr);
                DispatchTable::Continuations proof;
                proof.entries = &wait_entries.back();
                proof.count = 1;
                proof.restart_wait = wait_type;
                return DispatchTable::GeneratedBody(addr, [addr, body = std::move(body)](CpuContext& ctx, uint32_t a) {
                    const bool pushed = ShadowCallStack::Push(addr, ctx.cpuRegs.GPR.r[31].UL[0]);
                    body(ctx, a);
                    if (pushed) ShadowCallStack::Pop();
                }, proof);
            });
        // Rows whose address is not a function start of the pure-discovery
        // emission cannot be hooked: they run as part of their enclosing
        // generated function. Recorded (run.meta), never silently dropped.
        std::string not_starts;
        for (const auto& r : resolved)
            if (DispatchTable::IdOf(r.title.address) >= DispatchTable::FunctionCount())
                not_starts += (not_starts.empty() ? "" : ",") + r.title.name + "@" + Hex(r.title.address);
        PS2R_LOG(HOST, Info, "sdk manifest: %d rows installed (%d trap rows, %d not function starts: %s)", installed,
                 traps, unresolved, not_starts.c_str());
        if (auto* meta = host.Meta()) {
            meta->Set("sdk_rows_installed", std::to_string(installed));
            meta->Set("sdk_rows_trap", std::to_string(traps));   // installed rows with no engine body (park on first call)
            meta->Set("sdk_rows_not_fn_start", std::to_string(unresolved));
            meta->Set("sdk_rows_not_fn_start_list", not_starts);
        }
    }
    if (!input_save.Install([](uint32_t address, ::title::InputBindings::Body body) {
            return DispatchTable::Install(address, std::move(body));
        })) return 2;
    title::GameLibraries game_libraries(guest_mem, libmpeg,
        {cfg.hle.math_helpers, cfg.hle.movie_lifecycle, cfg.hle.sound_bank_headers,
         cfg.hle.stream_files || cfg.hle.sound_samples || cfg.hle.music, cfg.hle.culling},
        {
            [&](uint32_t location) {
                return movie_stream.OwnsCursor() ? movie_stream.Seek(location) :
                    static_cast<int32_t>(stream_reader.Seek(location));
            },
            read_movie_sectors
        },
        [&](uint16_t gain) { movie_sound.SetGain(gain); });
    ps2r::app::GraphicsApplication graphics_application(ram, guest_mem, game_libraries.Movie(), libmpeg,
        graphics_output, video, cfg, [&] { if (mtvu) mtvu->Stop(); }, s.aspect, s.session && *s.session != nullptr);
    // Refusal attribution stamps the leg's frame (the Verify boundary count) next
    // to the source Link ordinal: run.meta graphics_refusals_<family> first/last.
    graphics_application.SetFrameClock([&host] { return host.Boundaries(); });
    ps2r::app::FileServices file_services(guest_mem, iso_fs, cfg.hle.files);
    // An install refusal exits 2 with the refusing owner named (the owners print
    // their own reason first); the root never exits silently.
    const auto refused = [](const char* owner) {
        std::fprintf(stderr, "nativeee: %s refused installation\n", owner);
        return 2;
    };
    if (!game_libraries.Install()) return refused("game libraries");
    if (!graphics_application.Install()) return refused("Graphics application");
    if (!graphics_output.Install()) return refused("Graphics output");
    if (!file_services.Install()) return refused("file services");
    if (!effects_application.Install(
            [](uint32_t address) { return DispatchTable::Capture(address); },
            [](uint32_t address, DispatchTable::Body body, bool selected) {
                if (!selected)
                    body = DispatchTable::DelegateInterior(address, DispatchTable::Capture(address), std::move(body));
                return DispatchTable::Install(address, std::move(body));
            })) return refused("effects application");
    if (movie_stream.Install(
            [](uint32_t address) { return DispatchTable::Capture(address); },
            [](uint32_t address, DispatchTable::Body body) {
                return DispatchTable::Install(address, std::move(body));
            }, cfg.hle.movie_lifecycle) != 3) return refused("Movie stream");
    ps2r::app::MovieStorage movie_storage(game_libraries, movie_stream, iso_fs, cfg.hle.music_buffers);
    if (!sound_output.Install([&spu_core](uint32_t channel, uint16_t left, uint16_t right) {
            const uint32_t parameter = channel / 24 | (channel % 24) << 1;
            spu_core.SetParam(parameter, left);
            spu_core.SetParam(parameter | 0x100, right);
        })) return refused("sound output");
    if (!movie_storage.Install()) return refused("Movie storage");
    // The Movie player owners (Start/WaitFrame/Stop) need the complete Movie
    // service mask -- including the two buffer loans MovieStorage just
    // composed -- so they install last among the Movie services.
    if (!graphics_application.InstallMoviePlayer()) return refused("Movie player owners");
    if (!service_timeline.Install()) return refused("service timeline");
    // META-ARENA (HOOK_RECLASSIFICATION 0x181560): on uses the arena;
    // off uses the recompiled allocator and page pool as the console
    // keeps them -- and installs only the free body, so a restored image's nodes
    // already inside the arena are still freed there (run.meta meta_arena=).
    const int meta_arena_hooks = free_list_0x181560::InstallHooks(
        arena, [](uint32_t a) { return DispatchTable::Capture(a); },
        [](uint32_t a, free_list_0x181560::Body b) { return DispatchTable::Install(a, b); }, cfg.hle.meta_arena);
    // The FMV abort hold (HOOK_RECLASSIFICATION emulation-bug workaround): the
    // title's scene updater is wrapped so the premature abort word its pump
    // latches at the instant end of file is cleared while MpegApi still holds
    // pictures (the OLD observable of the frozen baseline; FmvAbortHold).
    FmvAbortHold fmv_abort_hold(guest_mem, fmv_layout, [&mpeg](uint32_t mpeg_addr) { return mpeg.ShouldHoldAbort(mpeg_addr); });
    if (!cfg.hle.movie_lifecycle &&
        !fmv_abort_hold.InstallHook([](uint32_t a) { return DispatchTable::Capture(a); },
                                   [](uint32_t a, FmvAbortHold::Body b) { return DispatchTable::Install(a, b); }))
        return 2;
    // The title's added option pages (MenuComposition; play mode only, ui.menu_pages).
    ps2r::app::MenuComposition menu(play_mode && cfg.ui.menu_pages ? session.get() : nullptr);
    if (menu.Refused()) return refused("menu extension");
    // RETAIL_BUGS fixes (A5, fixes.*; default off = nothing installed; play mode only).
    ps2r::app::FixesComposition retail_fixes(cfg, play_mode);
    if (retail_fixes.Refused()) return refused("retail fixes");
    // The title's pad readiness event: the boot UI's controller state machine
    // entering case 2 (TitleProfile::Pad) -> PadProtocol::NoteReadinessEvent.
    PadReadinessHook pad_readiness(pad_protocol, title.pad.ctrl_sm_entry, title.pad.ctrl_object_ptr,
                                   [](uint32_t a) { return memory::read<uint32_t>(a); }, pad_now);
    const bool pad_hook_installed = !input_save.InputSelected() && pad_readiness.Install(
        [](uint32_t a) { return DispatchTable::Capture(a); },
        [](uint32_t a, PadReadinessHook::Body b) { return DispatchTable::Install(a, b); });
    // The title's load-wait loop over LoaderModel's sync-wait rule: at its two
    // load-poll headroom queries (the lap-1 spin, the lap >= 2 single-poll gate)
    // the seam answers "no headroom" only when the noted poll was held in flight
    // and the read is delivered by the boundary model alone (LoaderSpinLift;
    // every other query keeps the real predicate).
    // Install the generated T0 headroom predicate before wrapping it: this is
    // a time query, and T0 remains constant inside a slice. LoaderSpinLift
    // supplies the load-wait exits.
    LoaderSpinLift loader_spin_lift(loader_model, [] { return LoaderFileIo::PollInFlight(); });
    frame_census.BindLift(loader_spin_lift);   // the per-frame census reads its reply-rule counters (data only)
    const bool headroom_query_recompiled =
        loader_spin_lift.GetLayout().fn_headroom_query == HeadroomQueryGlue::kAddress &&
        HeadroomQueryGlue::InstallRecompiled([](uint32_t a, HeadroomQueryGlue::Body b) { return DispatchTable::Install(a, b); });
    // Poll visibility: at the two engaged sites, a lap with no host-served read
    // in flight runs the IOP's work due inside the frame (the kernel to the frame
    // ceiling, no base advance; the EE-side service of a worker's bridged call)
    // and delivers the queued RPC end-callbacks -- the SIF interrupt and the
    // IOP's own clock of the console, pulled to the poll that asks. "Headroom"
    // is the IOP having RUN (a thread, a drive completion, a beat): a reply
    // alone does not hold the frame -- the title's status round trip answers
    // "still in progress" and re-asks every lap while the module's read is due
    // past the frame's edge, which on the console only time ends; here the next
    // IOP due past the ceiling is that end (the lift), and the boundary's pump
    // completes the read.
    loader_spin_lift.SetPumpWithinFrame([
#if PS2R_IOP_ORACLE
        &iop_table, &ee_rpc_server,
#endif
        &rpc_router](CpuContext& c) {
        bool ran = false;
#if PS2R_IOP_ORACLE
        ran = iop_table.PumpWithinFrame();
        if (ee_rpc_server.Service(c)) {
            iop_table.PumpWithinFrame();
            ran = true;
        }
#endif
        rpc_router.CompletePending(
            c, [](uint32_t addr, CpuContext& cc) { DispatchTable::Invoke(addr, cc); },
            [](uint32_t a) { return memory::read<uint32_t>(a); });
        return ran;
    });
    // The module round trip's cost (poll visibility): every STREAM.IRX RPC the
    // title issues advances the IOP's clock by the title's measured service time
    // (LoaderSpinLift::Layout::stream_rpc_service_us, the bridge charges it);
    // the lift sees the round trips and the IOP's instant inside the frame, and
    // at a reply lap runs the IOP's work due at or before that instant (the
    // module's drive completion once the instant passed it, the EE-side service,
    // the queued end-callbacks). Boundary mode charges nothing and binds nothing.
    if (cfg.cdvd.delivery_visibility == ps2r::CdvdDeliveryVisibility::Poll) {
        // A BLOCKING sceSifCallRpc (mode bit 0 clear) completes with its end
        // function already run (libkernl 3.0.1: the RPC-end handler runs it
        // before the waiter's semaphore is signalled) -- the title reads the
        // reply's processed copy right after Ship(0). The queued end callbacks
        // run on the caller's context at the call; boundary mode keeps the
        // slot-4 duty (its lanes are frozen).
        SceSif::SetBlockingEndCallbackRunner([&rpc_router](CpuContext& c) {
            rpc_router.CompletePending(
                c, [](uint32_t addr, CpuContext& cc) { DispatchTable::Invoke(addr, cc); },
                [](uint32_t a) { return memory::read<uint32_t>(a); });
        });
        // Service-only startup: the owned transport's served batches are the round
        // trips, its per-frame capacity the in-frame bound (NativeStartup::TimeLeft);
        // the retained arm (oracle binary) reads the bridge's completed tickets and
        // the IOP clock.
        const uint64_t rpc_service_us = loader_spin_lift.GetLayout().stream_rpc_service_us;
#if PS2R_IOP_ORACLE
        iop_table.Rpc().SetServiceUs(rpc_service_us);
        loader_spin_lift.SetIopInstant([&iop_table, &service_timeline, &native_startup, rpc_service_us] {
            LoaderSpinLift::IopInstant i;
            if (native_startup.Selected()) {
                i.round_trips = native_startup.RoundTrips() + service_timeline.Sequence();
                i.time_left = native_startup.TimeLeft(rpc_service_us, ps2r::app::ServiceTimeline::kFrameUs) && service_timeline.TimeLeft();
            } else {
                i.round_trips = iop_table.Rpc().Completed() + service_timeline.Sequence();
                i.time_left = service_timeline.TimeLeft();
            }
            return i;
        });
#else
        loader_spin_lift.SetIopInstant([&service_timeline, &native_startup, rpc_service_us] {
            LoaderSpinLift::IopInstant i;
            i.round_trips = native_startup.RoundTrips() + service_timeline.Sequence();
            i.time_left = native_startup.TimeLeft(rpc_service_us, ps2r::app::ServiceTimeline::kFrameUs) && service_timeline.TimeLeft();
            return i;
        });
#endif
        loader_spin_lift.SetPumpAtInstant([
#if PS2R_IOP_ORACLE
            &iop_table, &ee_rpc_server,
#endif
            &rpc_router](CpuContext& c) {
            bool ran = false;
#if PS2R_IOP_ORACLE
            ran = iop_table.PumpToInstant();
            if (ee_rpc_server.Service(c)) {
                iop_table.PumpToInstant();
                ran = true;
            }
#endif
            rpc_router.CompletePending(
                c, [](uint32_t addr, CpuContext& cc) { DispatchTable::Invoke(addr, cc); },
                [](uint32_t a) { return memory::read<uint32_t>(a); });
            return ran;
        });
    }
    loader_spin_lift.Install(
        [](uint32_t a) { return DispatchTable::Capture(a); },
        [](uint32_t a, LoaderSpinLift::Body b) { return DispatchTable::Install(a, b); });

    ps2r::app::ServiceTraceLog service_trace;
    if (!service_trace.Install(cfg.harness.out_dir.empty() ? "" : host.LegPath("services.log"),
                               domain,
#if PS2R_IOP_ORACLE
                               &iop_table,
#endif
                               delivery_meter)) return 2;
    // The services owner's startup contract read from the INSTALLED owners
    // (ServiceStartup::Current fills the library fields; the root names its own).
    // Today every module and the heap are retained (the batch transport and
    // .mb residency have no owner), so STREAM.IRX booted above keeps executing
    // and a live restore refuses with this named list; the load loop at 0x17ce50
    // and InitIopHeap stop executing only when the matching boolean is true.
    // The owners of service-only startup: the batch transport (the STREAM
    // server) and the effects/global-controls recorder, installed only under
    // hle.startup=native; ServiceStartup::Current reads their Enabled().
    {
        ::title::StreamTransport::Owners transport_owners;
        transport_owners.files = StreamLibrary::Enabled() && file_services.Enabled();
        transport_owners.banks = ::title::SoundBanks::Enabled();
        transport_owners.controls = ::title::SoundControls::Enabled();
        transport_owners.music = ::title::MusicLibrary::Enabled();
        transport_owners.buffers = ::title::MusicBuffers::Enabled();
        transport_owners.movie_pcm = movie_sound.StateOwner() != nullptr;
        ::title::StreamTransport::Install(native_startup.Selected(), transport_owners,
                                          [](uint32_t a, uint32_t v) { memory::write<uint32_t>(a, v); });
        ::title::SoundEffects::Install(native_startup.Selected());
    }
    ::title::ServiceStartup::Inputs startup_inputs;
    startup_inputs.disc_files = file_services.Enabled();
    startup_inputs.output = sound_output.OutputOwner() != nullptr;
    startup_inputs.movie_pcm = movie_sound.StateOwner() != nullptr;
    startup_inputs.movie_cursor = movie_stream.Selected();   // the selection, not a movie being open now
    ps2r::app::StartupOwnership startup_ownership(
        ::title::ServiceStartup::Inspect(::title::ServiceStartup::Current(startup_inputs)));
    PS2R_LOG(HOST, Info, "service startup: %s", startup_ownership.Census().c_str());
    s.game_libraries = &game_libraries;
    s.graphics_application = &graphics_application;
    s.file_services = &file_services;
    s.movie_storage = &movie_storage;
    s.meta_arena_hooks = &meta_arena_hooks;
    s.fmv_abort_hold = &fmv_abort_hold;
    s.menu_extension = menu.Extension();
    s.retail_fixes = retail_fixes.Fixes();
    s.pad_readiness = &pad_readiness;
    s.pad_hook_installed = &pad_hook_installed;
    s.loader_spin_lift = &loader_spin_lift;
    s.headroom_query_recompiled = &headroom_query_recompiled;
    s.service_trace = &service_trace;
    s.startup_ownership = &startup_ownership;
    return InstallChunkCensus(s);
}

int EeBootstrap::InstallChunkCensus(LegState& s) {
    // The title's chunk state machine as the load classifier (frame
    // skip): StepStates / StepChunkData wrapped, their states noted per
    // tick (TickProfile::NoteChunkEvent) -- the level-entry cut, the loading and
    // unloading chunks are load ticks; BoundaryProtocol's load windows need no
    // tail. Every profile (a read and a count; the bodies run unchanged). Its
    // own phase: the dispatch phase stands at the size budget.
    ChunkStateCensus chunk_state_census([](uint32_t a) { return memory::read<uint32_t>(a); });
    if (!chunk_state_census.InstallHooks([](uint32_t a) { return DispatchTable::Capture(a); },
                                         [](uint32_t a, ChunkStateCensus::Body b) { return DispatchTable::Install(a, b); })) {
        std::fprintf(stderr, "nativeee: chunk state census refused installation\n");
        return 2;
    }
    s.chunk_state_census = &chunk_state_census;
    return RegisterSections(s);
}
