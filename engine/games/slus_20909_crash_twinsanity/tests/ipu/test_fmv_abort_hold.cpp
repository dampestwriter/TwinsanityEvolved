// test_fmv_abort_hold -- the title's FmvAbortHold workaround class: the abort
// word is cleared only under every guard (a main-RAM context handle, the RAW8
// movie mode, a set abort word, MpegApi::ShouldHoldAbort true); inert otherwise
// (in particular when the predicate says the movie genuinely ended); the clear
// counter and the wrapper body that runs the generated updater after the tick.
#include <cstdint>
#include <cstdio>
#include <memory>

#include "tests/ipu/counting_fmv_pcm_sink.h"
#include "engine/ipu/hle/ffmpeg_video_decoder.h"
#include "engine/ipu/hle/mpeg_api.h"
#include "engine/ipu/ipu_settings.h"
#include "tests/ipu/file_fmv_sector_source.h"
#include "games/slus_20909_crash_twinsanity/hle/fmv/fmv_context_layout_data.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/fmv_abort_hold.h"
#include "tests/ipu/vector_guest_memory.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("  FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
        }                                                                  \
    } while (0)

constexpr uint32_t kCtx = 0x00400000u;   // a main-RAM context handle (inside the 8 MiB test memory)

std::unique_ptr<ps2r::ipu::VideoDecoder> MakeFfmpegDecoder() {
    return std::unique_ptr<ps2r::ipu::VideoDecoder>(new ps2r::ipu::FfmpegVideoDecoder());
}

// A context in the movie mode with the abort word latched, the handle word
// pointing at it.
void ArmMovie(VectorGuestMemory& mem, const ps2r::ipu::FmvContextLayout& l, uint32_t ctx = kCtx) {
    mem.Write32(FmvAbortHold::kContextHandleWord, ctx);
    mem.Write32(ctx + l.mode, l.mode_raw8);
    mem.Write32(ctx + l.abort_flag, 1);
}

void TestGuards() {
    const ps2r::ipu::FmvContextLayout l = FmvContextLayoutData::CrashTwinsanityUS();
    VectorGuestMemory mem(8u << 20);
    bool hold = true;
    uint32_t asked = 0;
    FmvAbortHold h(mem, l, [&](uint32_t a) { asked = a; return hold; });

    // No context handle: inert.
    CHECK(!h.Tick() && h.Holds() == 0, "no handle word: inert");
    CHECK(asked == 0, "predicate not consulted without a handle");

    // Handle outside main RAM (the kernel area / beyond 32 MiB): inert.
    mem.Write32(FmvAbortHold::kContextHandleWord, 0x8000u);
    mem.Write32(0x8000u + l.mode, l.mode_raw8);
    mem.Write32(0x8000u + l.abort_flag, 1);
    CHECK(!h.Tick() && mem.Read32(0x8000u + l.abort_flag) == 1, "handle below the main-RAM floor: inert");
    mem.Write32(FmvAbortHold::kContextHandleWord, 0x02000000u);
    CHECK(!h.Tick(), "handle at the 32 MiB ceiling: inert");

    // Movie mode but no abort latched: inert, predicate not consulted.
    ArmMovie(mem, l);
    mem.Write32(kCtx + l.abort_flag, 0);
    asked = 0;
    CHECK(!h.Tick() && asked == 0, "abort word clear: inert, predicate not consulted");

    // Abort latched but not the movie mode (RGB32 path, mode 1): inert.
    mem.Write32(kCtx + l.abort_flag, 1);
    mem.Write32(kCtx + l.mode, 1);
    CHECK(!h.Tick() && mem.Read32(kCtx + l.abort_flag) == 1, "mode != mode_raw8: the abort stands");

    // Every guard + the engine says pictures remain: the clear.
    mem.Write32(kCtx + l.mode, l.mode_raw8);
    CHECK(h.Tick(), "all guards + ShouldHoldAbort: cleared");
    CHECK(mem.Read32(kCtx + l.abort_flag) == 0, "abort word is 0 after the hold");
    CHECK(asked == kCtx, "predicate consulted with the context handle");
    CHECK(h.Holds() == 1, "one hold counted");
    // The next tick finds the word clear: no second hold.
    CHECK(!h.Tick() && h.Holds() == 1, "an already-clear word is not counted again");
    // The pump relatches it; the movie still has pictures: held again.
    mem.Write32(kCtx + l.abort_flag, 1);
    CHECK(h.Tick() && h.Holds() == 2, "a relatched abort is held again while pictures remain");

    // The movie genuinely ended (predicate false): the abort stands -- the
    // real end-of-movie teardown proceeds.
    hold = false;
    mem.Write32(kCtx + l.abort_flag, 1);
    CHECK(!h.Tick() && mem.Read32(kCtx + l.abort_flag) == 1, "ShouldHoldAbort false: the abort stands");
    CHECK(h.Holds() == 2, "no hold counted when the movie ended");
    CHECK(h.Ticks() == 9, "every Tick counted");
    std::printf("fmv_abort_hold guards: holds=%llu ticks=%llu\n", static_cast<unsigned long long>(h.Holds()),
                static_cast<unsigned long long>(h.Ticks()));
}

void TestHookBody() {
    const ps2r::ipu::FmvContextLayout l = FmvContextLayoutData::CrashTwinsanityUS();
    VectorGuestMemory mem(8u << 20);
    FmvAbortHold h(mem, l, [](uint32_t) { return true; });
    int orig_calls = 0;
    uint32_t orig_abort_seen = 0xFFFFFFFFu;
    uint32_t captured_addr = 0;
    uint32_t installed_addr = 0;
    FmvAbortHold::Body installed_body;
    const bool ok = h.InstallHook(
        [&](uint32_t addr) {
            captured_addr = addr;
            return FmvAbortHold::Body([&](CpuContext&, uint32_t) {
                ++orig_calls;
                orig_abort_seen = mem.Read32(kCtx + l.abort_flag);   // what the updater reads
            });
        },
        [&](uint32_t addr, FmvAbortHold::Body body) {
            installed_addr = addr;
            installed_body = body;
            return true;
        });
    CHECK(ok && h.Installed(), "InstallHook reports success");
    CHECK(captured_addr == FmvAbortHold::kSceneUpdater && installed_addr == FmvAbortHold::kSceneUpdater,
          "the wrapper captures and installs at the scene updater");

    CpuContext ctx{};
    ArmMovie(mem, l);
    installed_body(ctx, FmvAbortHold::kSceneUpdater);
    CHECK(orig_calls == 1, "the generated updater runs after the tick");
    CHECK(orig_abort_seen == 0, "the updater reads a cleared abort word");
    CHECK(h.Holds() == 1, "the wrapper counted the hold");
    // A tick with nothing to hold still runs the updater.
    installed_body(ctx, FmvAbortHold::kSceneUpdater);
    CHECK(orig_calls == 2 && h.Holds() == 1, "an inert tick still runs the updater");

    // A failed install is reported.
    FmvAbortHold h2(mem, l, [](uint32_t) { return true; });
    CHECK(!h2.InstallHook([](uint32_t) { return FmvAbortHold::Body(); }, [](uint32_t, FmvAbortHold::Body) { return false; }),
          "a refused install reports false");
    CHECK(!h2.Installed(), "Installed() false after a refused install");
}

// Over the real MpegApi: a context the engine has not fed (no input seen) says
// no hold -- the latched abort stands, the class is inert.
void TestInertOverMpegApi() {
    const ps2r::ipu::FmvContextLayout l = FmvContextLayoutData::CrashTwinsanityUS();
    VectorGuestMemory mem(8u << 20);
    FileFmvSectorSource src;   // never opened: no sectors
    CountingFmvPcmSink sink;
    ps2r::ipu::IpuSettings settings;
    ps2r::ipu::MpegApi api(mem, l, src, sink, MakeFfmpegDecoder, settings);
    api.Init();
    api.Reset(kCtx);
    FmvAbortHold h(mem, l, [&api](uint32_t a) { return api.ShouldHoldAbort(a); });
    ArmMovie(mem, l);
    CHECK(!api.ShouldHoldAbort(kCtx), "MpegApi: a context without input is not held");
    CHECK(!h.Tick() && h.Holds() == 0, "inert over MpegApi when ShouldHoldAbort is false");
    CHECK(mem.Read32(kCtx + l.abort_flag) == 1, "the abort word stands");
    api.Destroy(kCtx);
}

}  // namespace

int main() {
    TestGuards();
    TestHookBody();
    TestInertOverMpegApi();
    std::printf("test_fmv_abort_hold: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures) {
        std::printf("test_fmv_abort_hold: FAIL\n");
        return 1;
    }
    std::printf("test_fmv_abort_hold: PASS (guards, hook body, inert over MpegApi)\n");
    return 0;
}
