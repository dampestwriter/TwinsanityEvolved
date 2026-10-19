// te_vita.cpp -- the PlayStation Vita runner's start-up (te_vita.h).
#include "te_vita.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// newlib's heap (sbrk): the default is far too small for the title's 32 MB linear memory, the 32 MB game stack and the
// engine's caches. Weakly referenced by the SDK's sbrk; this definition sizes the block it allocates at start-up.
extern "C" unsigned int _newlib_heap_size_user = 224u * 1024u * 1024u;

namespace te::vita {

namespace {

constexpr const char* kDataDir = "ux0:data/twinsanity";

std::vector<std::string> g_args;
std::vector<char*> g_argv;
SceUID g_log_fd = -1;

int LogWrite(void*, const char* buf, int n) {
    // Both the file and the kernel's TTY (sceClibPrintf): an emulator shows the TTY in its own log at once, while its
    // host-side file buffer may reach the disk only at close.
    sceClibPrintf("%.*s", n, buf);
    if (g_log_fd < 0) return n;
    const int wrote = sceIoWrite(g_log_fd, buf, static_cast<SceSize>(n));
    sceIoSyncByFd(g_log_fd, 0);
    return wrote < 0 ? -1 : wrote;
}

// The default arguments: the disc image, the settings and the memory cards under the data folder; the boot movies
// skipped (the MPEG-2 path is untested on the device); the state trace on (the milestone's evidence); no keyboard.
std::vector<std::string> DefaultArgs() {
    return {"--iso", std::string(kDataDir) + "/game.iso", "--skip-movies", "--trace-state", "--no-kbm",
            "--mc-dir", std::string(kDataDir) + "/memcards", "--config", std::string(kDataDir) + "/ps2r.ini"};
}

// ux0:data/twinsanity/args.txt, when present, replaces the defaults: whitespace-separated tokens, # to end of line.
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
            if (i > start) out->push_back(line.substr(start, i - start));
        }
    }
    return !out->empty();
}

}  // namespace

void Startup(int* argc, char*** argv) {
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
