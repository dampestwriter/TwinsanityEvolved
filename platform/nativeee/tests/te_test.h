// te_test.h -- the host-side unit tests' harness: a 32 MB guest memory standing in for the runner's MainMemory binding,
// te::Settings per test, and a minimal CHECK. Each test is one executable (ctest) over the host bodies of one header.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "te_host.h"
#include "te_services.h"

#if defined(_WIN32)
// The UCRT has no setenv; the tests set process-local knobs (TE_GFX_HEADLESS) before the host bodies read them.
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value);
}
#endif

namespace te_test {

int& Failures();
int& Checks();

#define CHECK(cond)                                                                                         \
    do {                                                                                                    \
        ++te_test::Checks();                                                                                \
        if (!(cond)) {                                                                                      \
            ++te_test::Failures();                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                                   \
    } while (0)
#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        ++te_test::Checks();                                                                                \
        const auto va_ = (a);                                                                               \
        const auto vb_ = (b);                                                                               \
        if (!(va_ == vb_)) {                                                                                \
            ++te_test::Failures();                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,  \
                         static_cast<long long>(va_), static_cast<long long>(vb_));                        \
        }                                                                                                   \
    } while (0)

// Fresh services with these settings (deterministic clock by default: no pacer thread).
void Reset(te::Settings settings);
// The `void()` callbacks host bodies asked for through te_call_void_callback (the movie's present): count, last index.
uint32_t CallbacksCalled();
uint32_t LastCallback();
te::Settings DefaultSettings();

// Guest memory helpers
uint32_t WriteString(uint32_t address, const std::string& text);   // returns address
template <typename T> void WriteValue(uint32_t address, const T& v) { std::memcpy(te::GuestPointer(address, sizeof v), &v, sizeof v); }
template <typename T> T ReadValue(uint32_t address) { T v{}; std::memcpy(&v, te::GuestPointer(address, sizeof v), sizeof v); return v; }

// The fixture ISO (TE_TEST_ISO env / -D): empty when absent -- disc-backed checks are then skipped and reported.
std::string FixtureIso();

int Finish(const char* name);

}  // namespace te_test
