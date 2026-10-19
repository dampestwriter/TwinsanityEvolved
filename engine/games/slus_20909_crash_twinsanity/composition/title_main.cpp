// games/slus_20909_crash_twinsanity/composition/title_main.cpp -- the play composition root of
// this title: TitleMain() (engine/title/title_composition.h), handed argc/argv by app/host/main.cpp.
// Host-side composition lives here (config, logging, platform,
// crash handler, VerifyHost, FrameCap, ShutdownLadder); the EE composition
// (memory, ELF, dispatch, kernel, manifest, timing, savestate, park sink) is
// EeBootstrap's, called once per run.
//
// Composition:
//   ConfigLoader -> ConfigValidator (inside Load) -> Logger -> RingLog ->
//   DiagRegistry (diag builds) -> Platform -> CrashHandler -> VerifyHost ->
//   FrameCap -> ShutdownLadder -> EeBootstrap::Run (title files present) |
//   TickSource stub (no title files)
// The TickSource stub is the Phase-1 idle loop kept for a run without title
// files: it advances one virtual frame boundary per 16,667 virtual
// microseconds with no wall sleep under pacing=freerun, presenting a black
// native frame (hash of a 640x448 black scanout), pumping 800 stereo frames
// of silence per boundary into the audio lane and hashing the empty RAM
// image at the memory-checkpoint cadence.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "app/host/build_id.h"
#include "games/slus_20909_crash_twinsanity/composition/ee_bootstrap.h"
#include "app/host/first_run.h"
#include "app/host/game_files_stamp.h"
#include "app/host/iop_arm.h"
#include "app/host/launcher.h"
#include "app/host/savestate_slots.h"
#include "app/host/session_relaunch.h"
#include "app/host/shutdown_ladder.h"
#include "app/host/user_data.h"
#include "engine/core/config/config.h"
#include "engine/core/config/config_loader.h"
#include "engine/core/log/log.h"
#include "engine/core/log/logger.h"
#include "engine/core/log/ring_log.h"
#include "engine/core/savestate/savestate_format.h"
#include "engine/core/savestate/savestate_writer.h"
#include "engine/core/threads/park.h"
#include "engine/core/verify/frame_cap.h"
#include "engine/core/verify/verify_host.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/platform/posix/crash_handler.h"
#include "engine/platform/host/platform.h"
#include "engine/platform/platform.h"
#include "engine/title/title_composition.h"
#include "engine/title/title_profile.h"
#if PS2R_DIAGNOSTICS
#include "engine/core/diag/diag_registry.h"
#endif

#ifndef PS2R_TITLE_ID
#define PS2R_TITLE_ID "untitled"
#endif

namespace {

using namespace ps2r;
using ps2r::log::Logger;
using ps2r::ringlog::RingLog;

constexpr int      kNativeW = 640;
constexpr int      kNativeH = 448;
constexpr int      kMixFramesPerBoundary = 48000 / 60;   // 800 stereo frames
constexpr uint64_t kBoundaryUs = 16667;                  // one NTSC field
constexpr size_t   kRamBytes = 32u << 20;
constexpr size_t   kSpadBytes = 16u << 10;
constexpr size_t   kMetaArenaBytes = 2u << 20;

log::Level ToLoggerLevel(LogLevelSetting s) {
    switch (s) {
        case LogLevelSetting::Quiet: return log::Level::Quiet;
        case LogLevelSetting::Error: return log::Level::Error;
        case LogLevelSetting::Warn: return log::Level::Warn;
        case LogLevelSetting::Info: return log::Level::Info;
        case LogLevelSetting::Narrate: return log::Level::Narrate;
    }
    return log::Level::Quiet;
}

// The directory argv[0] names (empty when it carries none): the first leg of
// the input-file path rule (next to the executable, else paths.game_files).
std::string ExecutableDir(const char* argv0) {
    if (!argv0) return std::string();
    const std::string a = argv0;
    const size_t slash = a.find_last_of('/');
    return slash == std::string::npos ? std::string() : a.substr(0, slash);
}

// --load-state: the envelope is parsed and validated up front (magic,
// versions, CRCs) so a bad file fails before any subsystem exists; the
// section payloads are restored by EeBootstrap (StateRestorer).
bool ProbeLoadState(const std::string& path, uint32_t* vsync_frame, std::string* err) {
    savestate::FileMeta meta;
    if (!savestate::ReadStateFile(path, &meta, nullptr, err)) return false;
    *vsync_frame = meta.vsync_frame;
    return true;
}

// The Phase-1 idle loop. Returns the exit code when the frame cap ends the
// run (through FrameCap::ExitNow -- never returns in that case).
int RunTickSourceStub(const Config& cfg, verify::VerifyHost& host, verify::FrameCap& cap, uint32_t vsync_frame) {
    std::vector<uint8_t> black((size_t)kNativeW * kNativeH * 4, 0);
    std::vector<float>   silence((size_t)kMixFramesPerBoundary * 2, 0.0f);
    std::vector<uint8_t> ram(kRamBytes, 0), spad(kSpadBytes, 0), meta_arena(kMetaArenaBytes, 0);
    const std::vector<verify::MemorySpan> spans = {
        {ram.data(), ram.size()}, {spad.data(), spad.size()}, {meta_arena.data(), meta_arena.size()}};
    const bool freerun = cfg.engine.pacing == Pacing::Freerun;
    uint64_t virtual_us = 0;
    // realtime pacing waits through Park (the tree's only wait primitive);
    // deadlines are in Park's steady-clock domain.
    Park pacer;
    const uint64_t wall0 = Park::NowNs();
    for (;;) {
        virtual_us += kBoundaryUs;
        vsync_frame++;
        RingLog::SetFrame(vsync_frame);
        // Present (graphics seam) then the EE frame edge (audio pump + mem).
        host.OnPresent(vsync_frame, black.data(), kNativeW, kNativeH, 1, "none");
        host.OnBoundary(silence.data(), kMixFramesPerBoundary, spans);
        if (cap.OnBoundary()) cap.ExitNow(0);
        if (!cap.Armed() && !host.Armed()) return 0;   // nothing to run yet
        if (!freerun) {
            // realtime pacing: hold each boundary to its virtual instant. No
            // one notifies the pacer, so WaitUntil returns at the deadline.
            const uint64_t target = wall0 + virtual_us * 1000ull;
            while (Park::NowNs() < target) pacer.WaitUntil(target, pacer.Generation());
        }
    }
}

}  // namespace

using ps2r::log::Logger;
using ps2r::ringlog::RingLog;

int TitleMain(int argc, char** argv) {
    Config cfg;
    ConfigLoader loader;
    // The play binary has no IOP (iop_arm.h) -- hle.startup=retained and the
    // twelve degrade edges that fell back to it refuse; the oracle binary keeps them.
    loader.SetRetainedStartupAvailable(ps2r::app::kIopLinked);
    // C4: the user data root (UserData) is where ps2r.ini is read from and where
    // every empty paths.* directory resolves; `--set paths.user_data=` pins it.
    std::string legacy_root_note;
    const std::string platform_root = app::UserData::PlatformRoot(&legacy_root_note);
    loader.SetUserDataRoot(platform_root);
    const ConfigLoader::Result r = loader.Load(argc, argv, cfg);
    if (r.show_version) {
        std::printf("%s\n", g_build_id);
        return 0;
    }
    if (r.show_help) {
        std::printf("%s", ConfigLoader::Usage());
        return 0;
    }
    if (!r.ok) {
        std::fprintf(stderr, "nativeee: %s\n", r.error.c_str());
        return 2;
    }
    // The CLI route of the first-run extraction: verify the disc
    // image and lay the game files, memcards, settings and stamp under the
    // (resolved) user data root, then exit. No window, no game.
    if (!r.extract_disc.empty()) {
        const TitleProfile& title = ActiveTitleProfile();
        return app::FirstRun::RunCli(app::FirstRun::PlanFor(cfg, title, g_build_id, r.extract_disc, true));
    }

    // Logging: master level + file sink under paths.log_dir; ring masks and
    // capacity from Config (verbose tier only in diagnostics builds).
    const std::string log_dir = cfg.paths.log_dir.empty() ? std::string("logs") : cfg.paths.log_dir;
    Logger::Instance().Start(cfg.harness.headless ? std::string() : log_dir + "/runtime_log.txt",
                             ToLoggerLevel(cfg.dev.log_level), true);
    const uint32_t mask_normal = RingLog::ParseChannelList(cfg.log.channels.c_str(), ~0u);
#if PS2R_DIAGNOSTICS
    const uint32_t mask_verbose = mask_normal;
#else
    const uint32_t mask_verbose = 0;
#endif
    RingLog::Configure(mask_normal, mask_verbose, (uint32_t)cfg.log.ring_events, log_dir.c_str());
    RingLog::RegisterThread("main");
#if PS2R_DIAGNOSTICS
    diag::DiagRegistry::Instance().DumpConfig(stderr);
#endif

    platform::host::RegisterSelf();
    platform::host::PinCurrentThreadInteractive("ps2r-main");
    const std::string crash_ring = log_dir + "/ring_crash.rlog";
    platform::CrashHandler::Install(crash_ring.c_str());

    PS2R_LOG(HOST, Info, "nativeee %s config sources=%zu profile=%s", g_build_id, r.sources.size(),
             cfg.meta.profile.c_str());
    // The user data layout (C4): the root in use, the migrations from a
    // repository checkout's cwd-relative directories (copies, never moves) and
    // the BUILD-ID stamp of the extracted game files.
    PS2R_LOG(HOST, Info, "user data root: %s (settings %s)",
             cfg.paths.user_data.empty() ? "<none: cwd-relative defaults>" : cfg.paths.user_data.c_str(),
             app::UserData::SettingsFile(cfg).c_str());
    if (!legacy_root_note.empty()) {
        std::fprintf(stderr, "nativeee: %s\n", legacy_root_note.c_str());
        PS2R_LOG(HOST, Info, "%s", legacy_root_note.c_str());
    }
    for (const app::UserData::Migration& m : app::UserData::Migrate(cfg, ".")) {
        const std::string line = app::UserData::Describe(m);
        if (m.copied || !m.error.empty()) std::fprintf(stderr, "nativeee: %s\n", line.c_str());
        if (m.error.empty()) PS2R_LOG(HOST, Info, "%s", line.c_str());
        else PS2R_LOG(HOST, Warn, "%s", line.c_str());
    }
    const app::GameFilesStamp::Check stamp = app::GameFilesStamp::Verify(cfg.paths.game_files, g_build_id);
    if (const std::string warning = app::GameFilesStamp::Warning(stamp, g_build_id); !warning.empty()) {
        std::fprintf(stderr, "nativeee: WARNING: %s\n", warning.c_str());
        PS2R_LOG(HOST, Warn, "%s", warning.c_str());
    }
    // The dependency degradations (a prerequisite the user turned off): one
    // line each, on stderr and in the log, so a `--set hle.<key>=off` bisect
    // session states exactly which dependents went with it.
    for (const std::string& note : r.notes) {
        std::fprintf(stderr, "[config] %s\n", note.c_str());
        PS2R_LOG(HOST, Warn, "[config] %s", note.c_str());
    }

    // The launcher (first run: no game files -> drop the disc image,
    // verify, extract; afterwards the settings screen) runs BEFORE the game on a
    // plain start. Any harness flag bypasses it; --launcher forces it.
    if (app::Launcher::Wanted(cfg, ActiveTitleProfile(), r.harness_cli, r.launcher)) {
        app::Launcher launcher(cfg, ActiveTitleProfile(), g_build_id);
        if (!r.launcher_screenshot.empty()) launcher.SetScreenshot(r.launcher_screenshot, r.launcher_then_play);
        else if (r.launcher_then_play) launcher.SetAutoPlay();
        if (!r.launcher_disc.empty()) launcher.SetScriptedDrop(r.launcher_disc);
        if (!r.launcher_page.empty()) launcher.SetHarnessPage(r.launcher_page);
        const app::Launcher::Outcome outcome = launcher.Run();
        if (outcome == app::Launcher::Outcome::Quit) return 0;
        if (outcome == app::Launcher::Outcome::Failed) {
            std::fprintf(stderr, "nativeee: the launcher could not open or present a window; use --extract-disc <image> and --game-files\n");
            return 2;
        }
        PS2R_LOG(HOST, Info, "launcher: Play (game files %s, disc %s)", cfg.paths.game_files.c_str(), cfg.paths.iso.c_str());
    }

    uint32_t vsync_frame = 0;
    if (!cfg.harness.load_state.empty()) {
        std::string err;
        if (!ProbeLoadState(cfg.harness.load_state, &vsync_frame, &err)) {
            std::fprintf(stderr, "nativeee: --load-state %s: %s\n", cfg.harness.load_state.c_str(), err.c_str());
            return 2;
        }
        PS2R_LOG(SAVE, Info, "state %s: envelope ok, vsync_frame=%u", cfg.harness.load_state.c_str(),
                 vsync_frame);
        vsync_frame = 0;   // the idle stub counts its own frames from 1 (OLD legs key from 1 too)
    }

    verify::VerifyHost host;
    {
        // The decoder dependency is part of the leg's identity (CONSTRUCTION
        // section 4.7): folded into env_hash and stamped as run.meta
        // avcodec_version=; env_ps2 stays the Config fingerprint.
        std::string err;
        const std::vector<verify::VerifyHost::FingerprintEntry> provenance = {
            {"avcodec_version", ipu::FfmpegVideoDecoder::LibraryVersion()}};
        if (!host.Arm(cfg, g_build_id, PS2R_TITLE_ID, &err, provenance)) {
            std::fprintf(stderr, "nativeee: %s\n", err.c_str());
            return 2;
        }
        if (auto* meta = host.Meta()) {
            meta->Set("user_data_root", cfg.paths.user_data);
            meta->Set("game_files_stamp", app::GameFilesStamp::MetaValue(stamp));
        }
    }
    savestate::SavestateWriter state_writer;
    verify::FrameCap cap;
    cap.Arm((uint64_t)(cfg.harness.frames > 0 ? cfg.harness.frames : 0));
    cap.AddFinalizer([&] { host.Finalize(); });
    cap.AddFinalizer([&] { state_writer.JoinForExit(); });
    cap.AddFinalizer([] { RingLog::ExitDump(); });
    cap.AddFinalizer([] { Logger::Instance().Flush(); });

    const app::ShutdownLadder ladder(cfg.engine.shutdown_wait_s);
    (void)ladder;   // consumed by the thread teardown once guest threads exist

    // Phase 3: the EE composition root runs the leg (cold boot or restore) to
    // its park; the Phase-1 idle stub remains for a run with no title files.
    const TitleProfile& title = ActiveTitleProfile();
    EeBootstrap::Paths paths;
    paths.exe_dir = ExecutableDir(argv[0]);
    // The title directory (sdk_bindings.tsv, post-load patches, expected hashes):
    // the source tree's games/<title>/ when this binary runs from its build
    // (PS2R_TITLE_DIR is compiled in), else the artifact's staged copy beside
    // the binary -- dist/<platform>/title/ (build.sh / build.ps1),
    // found through the resolved executable path so a PATH launch works too.
    paths.title_dir = PS2R_TITLE_DIR;
    if (std::FILE* probe = std::fopen((paths.title_dir + "/" + title.sdk_bindings_file).c_str(), "rb")) {
        std::fclose(probe);
    } else {
        std::string exe_err;
        std::string exe = platform::host::ExecutablePath(&exe_err);
        const std::string exe_dir = exe.empty() ? paths.exe_dir : ExecutableDir(exe.c_str());
        const std::string beside = (exe_dir.empty() ? std::string(".") : exe_dir) + "/title";
        if (std::FILE* staged = std::fopen((beside + "/" + title.sdk_bindings_file).c_str(), "rb")) {
            std::fclose(staged);
            paths.title_dir = beside;
        } else {
            std::fprintf(stderr, "nativeee: title directory not found: neither %s nor %s has %s\n",
                         PS2R_TITLE_DIR, beside.c_str(), title.sdk_bindings_file.c_str());
        }
    }
    paths.elf = cfg.paths.game_files + "/" + title.elf_dir + "/" + title.elf_path;
    if (std::FILE* f = std::fopen(paths.elf.c_str(), "rb")) {
        std::fclose(f);
        const int code = EeBootstrap::Run(cfg, host, title, paths);
        cap.RunFinalizers();
        // F8 accepted as a relaunch (SessionRelaunch): the session has closed
        // and every finalizer ran; this process becomes the same binary with
        // the same arguments plus --load-state <slot> -- the composition's own
        // restore path, same pid (the bounded runner keeps following it).
        const std::string pending = app::SessionRelaunch::TakePending();
        if (!pending.empty()) {
            std::vector<std::string> original(argv, argv + argc);
            const auto stamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const std::vector<std::string> next = app::SavestateSlots::RelaunchArguments(
                original, pending, app::SavestateSlots::Root(cfg), stamp);
            std::string line;
            for (const std::string& a : next) line += (line.empty() ? "" : " ") + a;
            std::fprintf(stderr, "nativeee: savestate F8 restore Relaunch: %s\n", line.c_str());
            std::fflush(stderr);
            std::vector<char*> cargv;
            for (const std::string& a : next) cargv.push_back(const_cast<char*>(a.c_str()));
            cargv.push_back(nullptr);
            // The RUNNING image (resolved from the kernel; the preflight already
            // refused when it could not be), never argv[0]: execv does not search
            // PATH, and a bare name or a relative path after a chdir would fail
            // here with the session already gone. execvp(argv[0]) is the fallback.
            std::string image_error;
            const std::string image = app::SavestateSlots::RunningImage(&image_error);
            if (!image.empty()) execv(image.c_str(), cargv.data());
            const int exec_errno = errno;
            execvp(argv[0], cargv.data());
            std::fprintf(stderr,
                         "nativeee: savestate F8 restore Failed: exec %s: %s (execvp %s: %s) -- the live session is "
                         "already closed; nothing is running. Restart by hand: %s\n",
                         image.empty() ? ("<unresolved: " + image_error + ">").c_str() : image.c_str(),
                         image.empty() ? "not attempted" : std::strerror(exec_errno), argv[0], std::strerror(errno),
                         line.c_str());
            std::fflush(stderr);
            return 2;
        }
        return code;
    }
    PS2R_LOG(HOST, Warn, "ELF not found at %s: running the idle stub", paths.elf.c_str());
    const int code = RunTickSourceStub(cfg, host, cap, vsync_frame);
    cap.RunFinalizers();
    return code;
}
