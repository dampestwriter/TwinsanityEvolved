// platform/vita/platform_vita.cpp
#include "engine/platform/vita/platform_vita.h"
#include "engine/platform/host/platform.h"
#include "engine/platform/vita/fiber_vita.h"

#include <pthread.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace ps2r::platform {

namespace {

using ThreadFn = void (*)(void*);
struct ThreadStart {
    ThreadFn   fn;
    void*      arg;
    ThreadDesc desc;
};

void* ThreadTrampoline(void* p) {
    ThreadStart* s = static_cast<ThreadStart*>(p);
    if (s->desc.interactive_qos) PlatformVita::PinCurrentThreadInteractive(s->desc.name);
    else ps2r::host::SetCurrentThreadName(s->desc.name);
    ThreadFn fn = s->fn;
    void* arg = s->arg;
    delete s;
    fn(arg);
    return nullptr;
}

uint64_t NowNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

Thread ThreadCreate(void (*fn)(void*), void* arg, const ThreadDesc& desc) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (desc.stack) pthread_attr_setstacksize(&attr, desc.stack);
    pthread_t tid{};
    ThreadStart* s = new ThreadStart{fn, arg, desc};
    Thread t;
    if (pthread_create(&tid, &attr, &ThreadTrampoline, s) == 0) {
        t.handle = reinterpret_cast<void*>(tid);
    } else {
        delete s;
    }
    pthread_attr_destroy(&attr);
    return t;
}

void ThreadJoin(Thread t) {
    if (t.handle) pthread_join(reinterpret_cast<pthread_t>(t.handle), nullptr);
}

File Open(const char* path, FileMode mode) {
    const char* m = mode == FileMode::Read ? "rb" : mode == FileMode::Write ? "wb" : "ab";
    File f;
    f.fp = std::fopen(path, m);
    return f;
}

void CpuRelax() { std::this_thread::yield(); }

Platform Build() {
    Platform p;
    p.create_surface = &UnimplementedSurface;
    p.poll_input = &UnimplementedPollInput;
    p.audio_open = &UnimplementedAudioOpen;
    p.open = &Open;
    p.thread_create = &ThreadCreate;
    p.thread_join = &ThreadJoin;
    p.fiber_create = &FiberVita::Create;
    p.fiber_switch = &FiberVita::Switch;
    p.fiber_current = &FiberVita::Current;
    p.fiber_destroy = &FiberVita::Destroy;
    p.now_ns = &NowNs;
    p.timer_create = &UnimplementedTimerCreate;
    p.cpu_relax = &CpuRelax;
    p.map_reserve = &UnimplementedMapReserve;
    p.map_protect = &UnimplementedMapProtect;
    p.page_size = 4096;
    p.caps.name = "vita";
    p.caps.memory_budget_mb = 365;   // the extra-memory grant (ATTRIBUTE2=12): user + CDRAM + phycont
    p.caps.gles_level = 2;
    p.caps.has_fibers = true;
    p.caps.has_mmap = false;
    p.caps.has_window = false;   // sdl/
    p.caps.has_audio = false;    // sdl/
    return p;
}

}  // namespace

const Platform& PlatformVita::Instance() {
    static const Platform p = Build();
    return p;
}

void PlatformVita::RegisterSelf() { Register(&Instance()); }

void PlatformVita::PinCurrentThreadInteractive(const char* name, ThreadRole role) {
    ps2r::host::SetCurrentThreadName(name);
    static thread_local bool pinned = false;
    if (pinned) return;
    pinned = true;
    ps2r::host::PinCurrentThreadInteractive(role);
}

uint64_t PlatformVita::PhysFootprintBytes() { return ps2r::host::ProcessFootprintBytes(); }

std::string PlatformVita::ExecutablePath(std::string* err) { return ps2r::host::ExecutableImagePath(err); }

}  // namespace ps2r::platform
