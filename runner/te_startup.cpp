// runner/te_startup.cpp -- see te_startup.h.
#include "te_startup.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#endif

namespace te::startup {
namespace fs = std::filesystem;

namespace {

fs::path ExecutableDir() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return fs::path(buf).parent_path();
#elif defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return {};
    std::error_code ec;
    const fs::path p = fs::canonical(path, ec);
    return (ec ? fs::path(path) : p).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p.parent_path();
#else
    return {};
#endif
}

bool Blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; }

}  // namespace

std::string DiscImageBesideExecutable() {
    const fs::path dir = ExecutableDir();
    if (dir.empty()) return {};
    std::ifstream f(dir / kDiscImageFile, std::ios::binary);
    if (!f) return {};
    std::string line;
    std::getline(f, line);
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF)
        line.erase(0, 3);   // a UTF-8 byte-order mark from an editor
    size_t begin = 0, end = line.size();
    while (begin < end && Blank(line[begin])) ++begin;
    while (end > begin && Blank(line[end - 1])) --end;
    return line.substr(begin, end - begin);
}

void AttachParentConsole() {
#if defined(_WIN32)
    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_UNKNOWN) return;   // redirected: keep it
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;                               // started from a shortcut
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONOUT$", "w", stdout);
#endif
}

void ShowError(const char* title, const std::string& text) {
    std::fprintf(stderr, "[twinsanityevolved] %s: %s\n", title, text.c_str());
#if defined(_WIN32)
    if (GetConsoleWindow() == nullptr && GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN)
        MessageBoxA(nullptr, text.c_str(), title, MB_OK | MB_ICONERROR);
#endif
}

}  // namespace te::startup
