#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_source.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_oracle.h"

#include <cfenv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using S = title::ShadowSource;
using Matrix = ShadowOracle::Matrix;
size_t checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
template<class T> void Store(std::vector<uint8_t>& bytes, uint32_t at, const T& value) {
    CHECK(at <= bytes.size() && sizeof(T) <= bytes.size() - at);
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}
void CheckMatrices() {
    std::vector<uint8_t> bytes(0x400000);
    const title::MaterialState::Read read = [&](uint32_t at, void* out, size_t size) {
        if (at > bytes.size() || size > bytes.size() - at) return false;
        std::memcpy(out, bytes.data() + at, size);
        return true;
    };
    Store(bytes, S::kView, 0x160000u);
    Store(bytes, S::kTarget, 0x164000u);
    for (uint32_t height : {224u, 448u, 512u})
        for (unsigned variant = 0; variant < 8; ++variant) {
            const std::array<int32_t, 8> target{0, int32_t(variant) - 4, int32_t(variant) + 1,
                512 - int32_t(variant), int32_t(height) - int32_t(variant), 0, 512, int32_t(height)};
            Matrix camera{}, screen{};
            for (unsigned row = 0; row < 4; ++row)
                for (unsigned lane = 0; lane < 4; ++lane) {
                    camera[row][lane] = row == lane ? 1.f :
                        float((row + 1) * (lane + 1) * (variant + 1)) * .0037f;
                    screen[row][lane] = float(row * 11 + lane * 3) - 2.125f;
                }
            Store(bytes, 0x164000, target);
            Store(bytes, 0x160080, camera);
            Store(bytes, 0x1600c0, screen);
            std::fesetround(FE_TOWARDZERO);
            const auto expected = ShadowOracle::Matrices(512, height, target, camera, screen);
            S::BeginPlan plan;
            std::fesetround(FE_UPWARD);
            CHECK(S::Begin(read, {3, 512, height}, plan) == S::Result::Ok);
            CHECK(std::fegetround() == FE_UPWARD);
            std::fesetround(FE_TONEAREST);
            CHECK(plan.view == 0x160000 && plan.writes.size() == 4);
            constexpr std::array<uint32_t, 4> addresses{
                S::kSavedScreen, 0x1600c0, S::kShadowCamera, S::kShadowScreen};
            for (size_t i = 0; i < 4; ++i) {
                CHECK(plan.writes[i].address == addresses[i]);
                CHECK(plan.writes[i].bytes.size() == sizeof(Matrix));
                CHECK(std::memcmp(plan.writes[i].bytes.data(), expected.begin[i].data(), sizeof(Matrix)) == 0);
                std::memcpy(bytes.data() + addresses[i], plan.writes[i].bytes.data(), sizeof(Matrix));
            }
            S::Write restored;
            CHECK(S::End(read, plan.view, restored) == S::Result::Ok);
            CHECK(restored.address == 0x1600c0 && restored.bytes.size() == sizeof(Matrix));
            CHECK(std::memcmp(restored.bytes.data(), expected.end.data(), sizeof(Matrix)) == 0);
            CHECK(std::memcmp(restored.bytes.data(), screen.data(), sizeof(Matrix)) == 0);
        }
    S::BeginPlan sentinel;
    sentinel.view = 77;
    CHECK(S::Begin(read, {0, 32, 448}, sentinel) == S::Result::UnsupportedSource && sentinel.view == 77);
    Store(bytes, S::kView, 0u);
    CHECK(S::Begin(read, {0, 512, 512}, sentinel) == S::Result::InvalidMemory && sentinel.view == 77);
    Store(bytes, S::kView, 0xfff00000u);
    CHECK(S::Begin(read, {0, 512, 512}, sentinel) == S::Result::InvalidMemory && sentinel.view == 77);
    Store(bytes, S::kView, 0x160004u);
    CHECK(S::Begin(read, {0, 512, 512}, sentinel) == S::Result::InvalidMemory && sentinel.view == 77);
    Store(bytes, S::kView, 0x80160000u);
    CHECK(S::Begin(read, {0, 512, 512}, sentinel) == S::Result::Ok && sentinel.view == 0x160000);
    S::Write write{55, {1, 2, 3}};
    CHECK(S::End(read, 0x160100, write) == S::Result::UnsupportedSource && write.address == 55);
    Store(bytes, 0x160080, std::numeric_limits<float>::infinity());
    sentinel.view = 77;
    CHECK(S::Begin(read, {0, 512, 512}, sentinel) == S::Result::InvalidGeometry && sentinel.view == 77);
}
}
int main() {
    try {
        CheckMatrices();
        std::printf("ShadowSource: %zu checks, %llu complete NEW producer/helper calls, "
                    "same source matrix/target inputs\n", checks,
                    static_cast<unsigned long long>(ShadowOracle::Calls()));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
