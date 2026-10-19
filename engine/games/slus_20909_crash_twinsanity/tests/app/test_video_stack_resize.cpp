// tests/app/test_video_stack_resize.cpp -- VideoStackDefersOffMainResize: the
// VideoStack contract for a window resize posted from a thread other than the
// main thread (the GS consumer's SetWindow / ConfigureDisplay / the checkpoint's
// restore): it is RECORDED and never a synchronous call into the main thread,
// and the frame boundary (ApplyPendingWindowResize) carries it to the main
// thread through the session's dispatch.
//
// The deadlock this pins (int_work/.../allon_s4/relaunch_bisect, gs.queue=
// pipelined): the main thread in GraphicsOutput::SettleOwnedForCapture ->
// DrawIrThreadQueue::Sync() waits for the consumer; the consumer in OnJob ->
// VideoStack::OnWindowResizedOffMain -> SessionRuntime::OnMain waits for the
// main thread. The model here: the main dispatch stands for a main thread that
// cannot service a request (it records the attempt and refuses); the test
// thread blocks on the worker's completion exactly as Sync() does. No sleeps,
// no timeouts: a regression shows as a counted dispatch attempt (or, with a
// real main thread, the hang this replaces).
//
// Needs a VISIBLE window (headless stacks never touch OS geometry, so the
// deferral path is not reached); exit 77 (SKIP) when no window / GL context.
#include <atomic>
#include <cstdio>
#include <functional>
#include <future>
#include <thread>
#include <vector>

#include "games/slus_20909_crash_twinsanity/composition/video_stack.h"
#include "engine/core/config/config.h"
#include "engine/gs/vram/vram_image.h"

using namespace ps2r;

static int g_checks = 0, g_fail = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

int main() {
    gs::VramImage vram;
    VideoConfig video;
    video.scale = 1;
    video.vsync = false;
    app::VideoStack stack;
    if (!stack.Create(video, /*headless=*/false, "test_video_stack_resize", vram)) {
        std::printf("test_video_stack_resize: no window / GL context -> SKIP (77)\n");
        return 77;
    }
    const std::thread::id main_id = std::this_thread::get_id();

    // The session's main dispatch, modelled: a request from the main thread
    // itself runs inline (SessionRuntime::OnMain does the same); a request from
    // any other thread is the round trip the contract forbids -- counted and
    // refused (a main thread blocked in Sync() could never service it).
    std::atomic<int> inline_runs{0}, off_main_attempts{0};
    stack.SetSessionOutput(
        [](std::vector<uint8_t>, int, int) {},
        [&](std::function<void()> operation) {
            if (std::this_thread::get_id() == main_id) { operation(); inline_runs++; return true; }
            off_main_attempts++;
            return false;
        });
    stack.ReleaseCurrent();   // the consumer thread owns the context while it works

    // 1. A resize from a non-main thread while this (main) thread is blocked
    //    waiting for it: completes, recorded, no dispatch.
    {
        std::promise<void> finished;
        std::future<void> done = finished.get_future();
        std::thread consumer([&] {
            stack.EnsureCurrent();
            stack.OnWindowResizedOffMain(800, 600);
            stack.ReleaseCurrent();
            finished.set_value();
        });
        done.wait();   // the Sync() stand-in: main waits for the consumer, nothing else
        consumer.join();
    }
    CHECK(off_main_attempts.load() == 0, "OnWindowResizedOffMain made no call into the main thread");
    CHECK(inline_runs.load() == 0, "nothing applied before the boundary");
    CHECK(stack.DeferredResizes() == 1, "the resize was recorded (deferred_resizes=1)");

    // 2. A second off-main resize before the boundary coalesces (the last wins).
    {
        std::thread consumer([&] { stack.OnWindowResizedOffMain(1024, 768); });
        consumer.join();
    }
    CHECK(off_main_attempts.load() == 0, "still no call into the main thread");
    CHECK(stack.DeferredResizes() == 2, "both resizes counted");

    // 3. The boundary applies the pending geometry through the main dispatch
    //    (here: inline, this is the main thread) -- exactly once.
    stack.EnsureCurrent();
    stack.ApplyPendingWindowResize();
    CHECK(inline_runs.load() == 1, "the boundary dispatched one window resize to the main thread");
    stack.ApplyPendingWindowResize();
    CHECK(inline_runs.load() == 1, "an empty boundary applies nothing");
    CHECK(off_main_attempts.load() == 0, "the boundary never posted from off-main");

    // 4. The main-thread entry stays synchronous (its own contract): one
    //    dispatch, nothing deferred.
    stack.OnWindowResized(640, 448);
    CHECK(inline_runs.load() == 2, "OnWindowResized on the main thread dispatches at once");
    CHECK(stack.DeferredResizes() == 2, "OnWindowResized defers nothing");

    // 5. With a main POST installed (the play session), the boundary posts the
    //    geometry through it -- never the waiting dispatch (main may be inside a
    //    display swap
    std::atomic<int> posts{0};
    stack.SetSessionOutput(
        [](std::vector<uint8_t>, int, int) {},
        [&](std::function<void()> operation) { operation(); inline_runs++; return true; },
        [&](std::function<void()> operation) { operation(); posts++; return true; });
    {
        std::thread consumer([&] { stack.OnWindowResizedOffMain(800, 600); });
        consumer.join();
    }
    stack.ApplyPendingWindowResize();
    CHECK(posts.load() == 1, "the boundary posted the window resize through the main post");
    CHECK(inline_runs.load() == 2, "the waiting dispatch was not used by the boundary");
    stack.OnWindowResized(640, 448);
    CHECK(inline_runs.load() == 3 && posts.load() == 1, "the main-thread entry still dispatches synchronously");

    stack.Shutdown();
    std::printf("test_video_stack_resize: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
