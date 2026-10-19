#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_buckets.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using Buckets = title::GraphicsBuckets;
int checks = 0, failures = 0;
void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(value) Check((value), #value, __LINE__)

void Word(uint8_t* out, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

void TestSnapshot() {
    std::array<uint8_t, Buckets::kObjectBytes + 16> storage;
    storage.fill(0xee);
    for (size_t index = 0; index < 28; ++index) {
        auto* at = storage.data() + 8 + index * 0x38;
        Word(at, 0x100000 + static_cast<uint32_t>(index) * 0x100);
        Word(at + 4, 0x200000 + static_cast<uint32_t>(index) * 0x100);
        Word(at + 8, 0x300000 + static_cast<uint32_t>(index) * 0x100);
        at[0x0c] = 1;
        at[0x0e] = 2;
        Word(at + 0x10, index < 5 ? 2 : index < 21 ? 6 : 9);
        Word(at + 0x18, 0x11223344);
        Word(at + 0x1c, 0x55667788);
        Word(at + 0x24, 0x12345678);
        Word(at + 0x2c, 0x90abcdef);
        Word(at + 0x30, 0x76543210);
        Word(at + 0x34, 0x34567890);
    }
    const auto before = storage;
    unsigned reads = 0;
    Buckets::Reader reader = [&](uint32_t address, uint8_t* out, size_t count) {
        ++reads;
        CHECK(address == 0);
        CHECK(count == 0x628);
        std::memcpy(out, storage.data(), count);
        return true;
    };
    Buckets observer;
    Buckets::Snapshot result;
    CHECK(observer.Observe(0, 7, reader, &result) == Buckets::Result::Captured);
    CHECK(reads == 1);
    CHECK(storage == before);
    CHECK(result.address == 0);
    CHECK(result.from_interrupt);
    for (size_t i = 0; i < result.buckets.size(); ++i) {
        const auto& bucket = result.buckets[i];
        CHECK(bucket.first == 0x100000 + i * 0x100);
        CHECK(bucket.insertion == 0x200000 + i * 0x100);
        CHECK(bucket.last == 0x300000 + i * 0x100);
        CHECK(bucket.vu_buffer == 1 && bucket.program_region == 2);
        CHECK(bucket.chain == (i < 5 ? 2u : i < 21 ? 6u : 9u));
        CHECK(bucket.last_program_key == 0x5566778811223344ull);
        CHECK(bucket.last_2d_material == 0x12345678);
        CHECK(bucket.last_call == 0x90abcdef);
        CHECK(bucket.last_joints == 0x76543210);
        CHECK(bucket.last_slot == 0x34567890);
    }
    storage.fill(0);
    CHECK(result.buckets[27].first == 0x101b00);  // detached from reused guest storage
    CHECK(observer.Counts().observed_submissions == 1);
    CHECK(observer.Counts().failed_snapshots == 0);
}

void TestFailuresAndBoundary() {
    Buckets observer;
    Buckets::Snapshot output;
    output.address = 0x12345678;
    output.buckets[27].first = 0xabcdef01;
    unsigned reads = 0;
    auto reader = [&](uint32_t, uint8_t*, size_t) { ++reads; return false; };
    CHECK(observer.Observe(1, 0, reader, &output) == Buckets::Result::InvalidRange);
    CHECK(observer.Observe(0xfffffffcu, 0, reader, &output) == Buckets::Result::InvalidRange);
    CHECK(reads == 0);
    CHECK(observer.Observe(0, 0, {}, &output) == Buckets::Result::Unavailable);
    CHECK(observer.Observe(0, 0, reader, &output) == Buckets::Result::Unavailable);
    CHECK(reads == 1);
    auto throws = [](uint32_t, uint8_t*, size_t) -> bool { throw std::runtime_error("unmapped"); };
    CHECK(observer.Observe(0, 0, throws, &output) == Buckets::Result::Unavailable);
    CHECK(output.address == 0x12345678);
    CHECK(output.buckets[27].first == 0xabcdef01);
    const uint32_t last = std::numeric_limits<uint32_t>::max() -
                          static_cast<uint32_t>(Buckets::kObjectBytes - 1);
    auto edge = [&](uint32_t address, uint8_t* out, size_t bytes) {
        CHECK(address == last);
        std::memset(out, 0, bytes);
        return true;
    };
    CHECK(observer.Observe(last, 0, edge, &output) == Buckets::Result::Captured);
    CHECK(!output.from_interrupt);
    CHECK(observer.Counts().observed_submissions == 6);
    CHECK(observer.Counts().failed_snapshots == 5);
    CHECK(Buckets::kSubmitAddress == 0x0017ed50);
    CHECK(Buckets::kStripAddress == 0x001a9120);
}
}  // namespace

int main() {
    TestSnapshot();
    TestFailuresAndBoundary();
    std::printf("GraphicsBuckets: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
