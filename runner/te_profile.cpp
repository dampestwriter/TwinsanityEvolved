// te_profile.cpp -- TE_PROFILE=FILE: where the frame time goes, on the machine that plays.
//
// A sampling thread stops every other thread of the process about a thousand times a second, notes where it is and walks
// its stack (the x64 unwind tables, RtlVirtualUnwind), and lets it go. At exit FILE gets, per thread that was sampled
// running, its sample count and two tables: the functions it was IN (self) and the functions on its stack (inclusive, a
// function counted once per sample) -- the game's own functions under their wasm2c names (w2c_game_*), the engine's and
// the platform layer's under theirs. Names need a build with symbols (a PDB beside the program); without one the tables
// carry addresses. TE_PROFILE_HZ sets the rate (default 1000). Windows only; elsewhere a sampling profiler of the system
// (perf, Instruments) does the same.
#include "te_profile.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/platform/host/platform.h"

namespace te::profile {
namespace {

struct Counts {
    std::wstring name;
    uint64_t samples = 0;
    std::unordered_map<uint64_t, uint64_t> self;        // function start -> samples in it
    std::unordered_map<uint64_t, uint64_t> inclusive;   // function start -> samples with it on the stack
};

std::string g_path;
int g_hz = 1000;
std::atomic<bool> g_stop{false};
std::thread g_sampler;
std::atomic<DWORD> g_sampler_id{0};
std::unordered_map<DWORD, Counts> g_threads;

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

std::wstring ThreadName(HANDLE thread) {
    static const auto fn = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    if (!fn) return L"";
    PWSTR text = nullptr;
    std::wstring name;
    if (SUCCEEDED(fn(thread, &text)) && text) {
        name = text;
        LocalFree(text);
    }
    return name;
}

// The function an address lies in (its unwind entry's start), or the address itself when it has none (a leaf, a stub)
uint64_t FunctionOf(uint64_t pc) {
    DWORD64 base = 0;
    const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, nullptr);
    return f ? base + f->BeginAddress : pc;
}

// While a thread is stopped nothing may be called that takes a lock it could be holding (the heap's, the loader's: the
// unwind tables' lookup takes the latter), so the stopped thread is only copied -- its registers and the top of its stack
// -- and the copy is unwound after it runs again, the stack pointers moved into the copy.
alignas(16) uint8_t g_stack[64 * 1024];

void Sample(DWORD id, HANDLE thread) {
    if (SuspendThread(thread) == DWORD(-1)) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    size_t copied = 0;
    if (GetThreadContext(thread, &ctx)) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(ctx.Rsp), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
            const uint64_t end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
            copied = size_t(std::min<uint64_t>(end - ctx.Rsp, sizeof g_stack));
            std::memcpy(g_stack, reinterpret_cast<const void*>(ctx.Rsp), copied);
        }
    }
    ResumeThread(thread);
    if (copied < 8) return;
    const uint64_t lo = ctx.Rsp, hi = ctx.Rsp + copied;
    const uint64_t delta = reinterpret_cast<uint64_t>(g_stack) - lo;
    auto inCopy = [&](uint64_t a) { return a >= lo && a < hi; };
    uint64_t frames[48];
    int n = 0;
    frames[n++] = ctx.Rip;
    ctx.Rsp += delta;
    if (inCopy(ctx.Rbp)) ctx.Rbp += delta;
    const uint64_t copyLo = reinterpret_cast<uint64_t>(g_stack), copyHi = copyLo + copied;
    while (n < 48) {
        if (ctx.Rsp < copyLo || ctx.Rsp + 8 > copyHi) break;
        DWORD64 base = 0;
        const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!f) {   // a leaf function: its return address is on top of the stack
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &handler_data, &establisher, nullptr);
            if (inCopy(ctx.Rsp)) ctx.Rsp += delta;   // restored from a frame pointer saved in the original stack
            if (inCopy(ctx.Rbp)) ctx.Rbp += delta;
        }
        if (ctx.Rip == 0) break;
        frames[n++] = ctx.Rip;
    }
    Counts& c = g_threads[id];
    if (c.samples == 0) c.name = ThreadName(thread);
    c.samples++;
    c.self[FunctionOf(frames[0])]++;
    uint64_t seen[48];
    int unique = 0;
    for (int i = 0; i < n; ++i) {
        const uint64_t fn = FunctionOf(frames[i]);
        if (std::find(seen, seen + unique, fn) != seen + unique) continue;
        seen[unique++] = fn;
        c.inclusive[fn]++;
    }
}

void Run() {
    const DWORD self = GetCurrentProcessId();
    std::unordered_map<DWORD, HANDLE> handles;
    ULONGLONG listed = 0;
    const DWORD period_us = DWORD(1000000 / std::max(1, g_hz));
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    while (!g_stop.load(std::memory_order_relaxed)) {
        if (GetTickCount64() - listed > 250) {   // threads come and go: re-list them four times a second
            listed = GetTickCount64();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{};
                te.dwSize = sizeof te;
                std::unordered_set<DWORD> alive;
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != self || te.th32ThreadID == g_sampler_id) continue;
                    alive.insert(te.th32ThreadID);
                    if (!handles.count(te.th32ThreadID)) {
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                              te.th32ThreadID);
                        if (h) handles[te.th32ThreadID] = h;
                    }
                }
                CloseHandle(snap);
                for (auto it = handles.begin(); it != handles.end();) {
                    if (!alive.count(it->first)) { CloseHandle(it->second); it = handles.erase(it); }
                    else ++it;
                }
            }
        }
        for (const auto& [id, h] : handles) Sample(id, h);
        next.QuadPart += freq.QuadPart * period_us / 1000000;
        QueryPerformanceCounter(&now);
        if (now.QuadPart < next.QuadPart) {
            const DWORD ms = DWORD((next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart);
            Sleep(ms);
        } else {
            next = now;
        }
    }
    for (const auto& [id, h] : handles) CloseHandle(h);
}

std::string Name(uint64_t fn) {
    std::string s = ps2r::host::SymbolForAddress(reinterpret_cast<const void*>(fn));
    if (s.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(fn));
        s = buf;
    }
    return s;
}

void Table(FILE* f, const char* title, const std::unordered_map<uint64_t, uint64_t>& counts, uint64_t total, size_t rows) {
    std::vector<std::pair<uint64_t, uint64_t>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(f, "  %s\n", title);
    for (size_t i = 0; i < v.size() && i < rows; ++i)
        std::fprintf(f, "    %6.2f%%  %8llu  %s\n", 100.0 * double(v[i].second) / double(total), static_cast<unsigned long long>(v[i].second),
                     Name(v[i].first).c_str());
}

void Stop() {
    if (!g_sampler.joinable()) return;
    g_stop = true;
    g_sampler.join();
    FILE* f = std::fopen(g_path.c_str(), "w");
    if (!f) return;
    std::vector<std::pair<DWORD, const Counts*>> threads;
    for (const auto& [id, c] : g_threads) threads.emplace_back(id, &c);
    std::sort(threads.begin(), threads.end(), [](const auto& a, const auto& b) { return a.second->samples > b.second->samples; });
    std::fprintf(f, "TE_PROFILE: %d Hz, %zu threads sampled\n", g_hz, threads.size());
    for (const auto& [id, c] : threads) {
        std::string name(c->name.begin(), c->name.end());
        std::fprintf(f, "\nthread %lu %s: %llu samples\n", static_cast<unsigned long>(id), name.empty() ? "(unnamed)" : name.c_str(),
                     static_cast<unsigned long long>(c->samples));
        Table(f, "self (the function the thread was in)", c->self, c->samples, 40);
        Table(f, "inclusive (the function on the thread's stack)", c->inclusive, c->samples, 60);
    }
    std::fclose(f);
    std::fprintf(stderr, "[twinsanityevolved] profile written to %s\n", g_path.c_str());
}

}  // namespace

void StartFromEnvironment() {
    const char* path = std::getenv("TE_PROFILE");
    if (!path || !*path) return;
    g_path = path;
    if (const char* hz = std::getenv("TE_PROFILE_HZ")) g_hz = std::max(10, std::atoi(hz));
    g_sampler = std::thread([] {
        g_sampler_id = GetCurrentThreadId();
        Run();
    });
    while (g_sampler_id == 0) Sleep(1);
    std::atexit(Stop);
    std::fprintf(stderr, "[twinsanityevolved] profiling at %d Hz into %s\n", g_hz, g_path.c_str());
}

}  // namespace te::profile

#else

namespace te::profile {
void StartFromEnvironment() {}
}  // namespace te::profile

#endif
