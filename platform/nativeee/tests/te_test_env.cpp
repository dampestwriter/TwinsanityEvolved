// te_test_env.cpp -- the runner's symbols the host bodies need (te::GuestMemory & co.), over a 32 MB test buffer.
#include "te_test.h"

#include <algorithm>
#include <csetjmp>
#include <vector>

namespace {
std::vector<uint8_t> g_memory(32u << 20);
int g_failures = 0, g_checks = 0;
}  // namespace

namespace te {
uint8_t* GuestMemory() { return g_memory.data(); }
std::size_t GuestMemorySize() { return g_memory.size(); }
const char* GuestString(uint32_t address, std::size_t max) {
    const uint8_t* p = GuestPointer(address, 1);
    if (!p) return nullptr;
    const std::size_t room = std::min<std::size_t>(max, GuestMemorySize() - address);
    return std::memchr(p, 0, room) ? reinterpret_cast<const char*>(p) : nullptr;
}
[[noreturn]] void RequestExit(int status) {
    std::fprintf(stderr, "  RequestExit(%d) in a test\n", status);
    std::exit(status == 0 ? 99 : status);
}
}  // namespace te

// The movie player's present callback (a wasm table index) has no module to land in here: counted, so a test can
// see the player asked for it (te_test::CallbacksCalled).
namespace { uint32_t g_callbacks = 0, g_last_callback = 0; }
extern "C" int te_call_void_callback(uint32_t index) {
    ++g_callbacks;
    g_last_callback = index;
    return 1;
}

namespace te_test {

uint32_t CallbacksCalled() { return g_callbacks; }
uint32_t LastCallback() { return g_last_callback; }

int& Failures() { return g_failures; }
int& Checks() { return g_checks; }

te::Settings DefaultSettings() {
    te::Settings s;
    s.deterministic = true;
    s.sdl_input = false;
    s.iso_path = FixtureIso();
    return s;
}

void Reset(te::Settings settings) {
    te::Services::ResetForTest();
    te::SetSettings(settings);
    std::fill(g_memory.begin(), g_memory.end(), 0);
}

uint32_t WriteString(uint32_t address, const std::string& text) {
    uint8_t* p = te::GuestPointer(address, text.size() + 1);
    std::memcpy(p, text.c_str(), text.size() + 1);
    return address;
}

std::string FixtureIso() {
    if (const char* env = std::getenv("TE_TEST_ISO")) return env;
#ifdef TE_TEST_ISO
    return TE_TEST_ISO;
#else
    return "";
#endif
}

int Finish(const char* name) {
    std::fprintf(stderr, "%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace te_test
