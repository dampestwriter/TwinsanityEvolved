// te_vita.cpp -- the PlayStation Vita runner's start-up (te_vita.h).
#include "te_vita.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// newlib's heap (sbrk): the default is far too small for the title's 32 MB linear memory, the 32 MB game stack and the
// engine's caches. Weakly referenced by the SDK's sbrk; this definition sizes the block it allocates at start-up.
extern "C" unsigned int _newlib_heap_size_user = 224u * 1024u * 1024u;
// Every thread the tree starts (std::thread: the render thread, the geometry builders, the audio feeder) takes the
// pthread default stack, 32 KB unless this says otherwise -- the builders' VU programs and strip buffers outgrow that
// and overwrote the memory beside the stack (mutexes included). Weakly referenced by the SDK's pthread_create.
extern "C" unsigned int _pthread_stack_default_user = 1024u * 1024u;

namespace te::vita {

namespace {

constexpr const char* kDataDir = "ux0:data/twinsanity";

std::vector<std::string> g_args;
std::vector<char*> g_argv;
SceUID g_log_fd = -1;
int g_synced_writes = 0;
SceUInt64 g_last_sync_us = 0;

// The start-up's lines are synced one by one (a crash before the first frame still leaves them on the card); after
// that a sync per line would stall the game thread on the memory card's flash whenever something logs mid-play, so the
// file is synced at most every few seconds
constexpr int kSyncedWrites = 400;
constexpr SceUInt64 kSyncIntervalUs = 5000000;

int LogWrite(void*, const char* buf, int n) {
    // Both the file and the kernel's TTY (sceClibPrintf): an emulator shows the TTY in its own log at once, while its
    // host-side file buffer may reach the disk only at close.
    sceClibPrintf("%.*s", n, buf);
    if (g_log_fd < 0) return n;
    const int wrote = sceIoWrite(g_log_fd, buf, static_cast<SceSize>(n));
    const SceUInt64 now = sceKernelGetProcessTimeWide();
    if (g_synced_writes < kSyncedWrites || now - g_last_sync_us >= kSyncIntervalUs) {
        ++g_synced_writes;
        g_last_sync_us = now;
        sceIoSyncByFd(g_log_fd, 0);
    }
    return wrote < 0 ? -1 : wrote;
}

// The default arguments: the disc image, the settings and the memory cards under the data folder; the boot movies
// skipped (the MPEG-2 path is untested on the device); the state trace on (the milestone's evidence); no keyboard.
std::vector<std::string> DefaultArgs() {
    return {"--iso", std::string(kDataDir) + "/game.iso", "--skip-movies", "--trace-state", "--no-kbm",
            "--mc-dir", std::string(kDataDir) + "/memcards", "--config", std::string(kDataDir) + "/ps2r.ini"};
}

// ux0:data/twinsanity/args.txt, when present, replaces the defaults: whitespace-separated tokens, # to end of line. A
// token NAME=value whose name starts with TE_ or PS2E_ sets that environment variable instead (the device has no shell to
// set the tree's diagnostic switches from).
bool ReadArgsFile(std::vector<std::string>* out) {
    std::ifstream f(std::string(kDataDir) + "/args.txt");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            const size_t start = i;
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i == start) continue;
            const std::string token = line.substr(start, i - start);
            const size_t eq = token.find('=');
            if (eq != std::string::npos && (token.rfind("TE_", 0) == 0 || token.rfind("PS2E_", 0) == 0)) {
                setenv(token.substr(0, eq).c_str(), token.substr(eq + 1).c_str(), 1);
                std::fprintf(stderr, "[twinsanityevolved] vita: %s from args.txt\n", token.c_str());
                continue;
            }
            out->push_back(token);
        }
    }
    return !out->empty();
}

}  // namespace

void Startup(int* argc, char*** argv) {
    // The highest clocks an application may ask for (the system starts it at 333 MHz ARM / 111 MHz GPU): the game's
    // frame needs every cycle the four cores and the GPU have
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(kDataDir, 0777);
    sceIoMkdir((std::string(kDataDir) + "/memcards").c_str(), 0777);
    // The log: a kernel descriptor written and SYNCED per line (the emulator's host file buffer is otherwise flushed
    // only at close, which a crashed run never reaches), behind newlib's stdio through funopen so every fprintf(stderr)
    // / printf in the tree lands there unchanged.
    const std::string log = std::string(kDataDir) + "/twinsanityevolved.log";
    g_log_fd = sceIoOpen(log.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (g_log_fd >= 0) {
        if (FILE* f = funopen(nullptr, nullptr, &LogWrite, nullptr, nullptr)) {
            std::setvbuf(f, nullptr, _IOLBF, 0);
            stderr = f;
            stdout = f;
        }
    }

    SceKernelFreeMemorySizeInfo mem{};
    mem.size = sizeof mem;
    if (sceKernelGetFreeMemorySize(&mem) >= 0)
        std::fprintf(stderr, "[twinsanityevolved] vita: free memory at start-up: user %d KB, cdram %d KB, phycont %d KB; newlib heap %u MB\n",
                     mem.size_user / 1024, mem.size_cdram / 1024, mem.size_phycont / 1024, _newlib_heap_size_user >> 20);
    std::fprintf(stderr, "[twinsanityevolved] vita: clocks ARM %d MHz, bus %d MHz, GPU %d MHz, crossbar %d MHz\n",
                 scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
                 scePowerGetGpuXbarClockFrequency());

    g_args.clear();
    const bool from_file = ReadArgsFile(&g_args);
    if (!from_file) g_args = DefaultArgs();
    g_argv.clear();
    g_argv.push_back((*argv && (*argv)[0]) ? (*argv)[0] : const_cast<char*>("twinsanityevolved"));
    for (std::string& a : g_args) g_argv.push_back(a.data());
    g_argv.push_back(nullptr);
    *argc = static_cast<int>(g_argv.size()) - 1;
    *argv = g_argv.data();
    std::fprintf(stderr, "[twinsanityevolved] vita: arguments (%s):", from_file ? "ux0:data/twinsanity/args.txt" : "built in");
    for (const std::string& a : g_args) std::fprintf(stderr, " %s", a.c_str());
    std::fprintf(stderr, "\n");
}

}  // namespace te::vita
