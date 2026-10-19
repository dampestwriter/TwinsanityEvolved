#include <algorithm>
#include <cstdio>
#include <limits>
#include <unistd.h>

#include "tests/cdvd/stream_library_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/services/files_fixture.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)
using F = title::Files;
using R = FilesFixture;

void Compare(const std::string& path) {
    R model(path, false), owned(path, true);
    CHECK(model.ready && owned.ready);
    for (const char* name : {"data.bd", "cdrom0:\\DATA.BD;1", "/DaTa.bD"}) {
        model.Text(name); owned.Text(name);
        const auto a = uint32_t(model.Call(F::kOpen, {R::kPath, 1}));
        const auto b = uint32_t(owned.Call(F::kOpen, {R::kPath, 1}));
        CHECK(int32_t(a) >= 0 && int32_t(b) >= 0);
        // Descriptors are opaque; compare cursor/bytes/results after resolving
        // each arm's own handle. Include unaligned reads and exact/past EOF.
        for (uint32_t position : {0u, 1u, 17u, 2047u, 4095u, stream_fixture::kSize - 1}) {
            CHECK(model.Call(F::kSeek, {a, position, 0}) == owned.Call(F::kSeek, {b, position, 0}));
            for (uint32_t bytes : {0u, 1u, 31u, 2048u, 6007u}) {
                std::fill(model.ram.begin() + R::kOutput, model.ram.end(), 0xbc);
                std::fill(owned.ram.begin() + R::kOutput, owned.ram.end(), 0xbc);
                const auto got = model.Call(F::kRead, {a, R::kOutput + 3, bytes});
                CHECK(got == owned.Call(F::kRead, {b, R::kOutput + 3, bytes}));
                CHECK(model.ram == owned.ram);
                CHECK(model.Call(F::kSeek, {a, 0, 1}) == owned.Call(F::kSeek, {b, 0, 1}));
            }
        }
        CHECK(model.Call(F::kSeek, {a, uint32_t(-19), 2}) ==
              owned.Call(F::kSeek, {b, uint32_t(-19), 2}));
        CHECK(model.Call(F::kRead, {a, R::kOutput, 64}) ==
              owned.Call(F::kRead, {b, R::kOutput, 64}));
        CHECK(model.ram == owned.ram);
        CHECK(model.Call(F::kClose, {a}) == owned.Call(F::kClose, {b}));
    }
    CHECK(model.model_calls > 0 && owned.model_calls == 0 && owned.model.OpenFiles() == 0);
    CHECK((owned.files.Count().bytes > 0 &&
           owned.files.Count().original_calls == std::array<uint64_t, 6>{}));
    // An interior restored frame stays with its original SDK stack owner.
    CpuContext interior{};
    interior.cpuRegs.GPR.r[4].SD[0] = -1;
    owned.dispatch.at(F::kClose)(interior, F::kClose + 8);
    CHECK(owned.model_calls == 1 && owned.files.Count().original_calls[2] == 1);
    std::printf("Files model/replacement bytes=%llu\n",
                static_cast<unsigned long long>(owned.files.Count().bytes));
}

void Errors(const std::string& path) {
    R r(path, true);
    CHECK(r.ready);
    // OLD's substring fake-open must never make a nonexistent file readable.
    r.Text("cdrom0:\\missing_slus_209.09.backup");
    CHECK(r.Call(F::kOpen, {R::kPath, 1}) < 0);
    r.Text("data.bd");
    const auto fd = uint32_t(r.Call(F::kOpen, {R::kPath, 1}));
    CHECK(int32_t(fd) >= 0);
    for (uint32_t flags : {0u, 2u, 3u, 0x201u, 0x401u, 0x101u})
        CHECK(r.Call(F::kOpen, {R::kPath, flags}) < 0);
    CHECK(r.Call(F::kWrite, {fd, 0xffffffff, 32}) == -30);
    CHECK(r.Call(F::kSeek, {fd, 1, 2}) < 0);
    CHECK(r.Call(F::kSeek, {fd, uint32_t(-1), 0}) < 0);
    CHECK(r.Call(F::kSeek, {fd, 0, 3}) < 0);
    CHECK(r.Call(F::kRead, {fd, R::kOutput, uint32_t(-1)}) < 0);
    CHECK(r.Call(F::kRead, {fd, uint32_t(r.ram.size() - 4), 32}) < 0);
    CHECK(r.Call(F::kRead, {fd, 0, 32}) < 0);
    CHECK(r.Call(F::kSeek, {fd, 0, 1}) == 0);
    CHECK(r.Call(F::kClose, {fd}) == 0);
    CHECK(r.Call(F::kClose, {fd}) == -9);
    CHECK(r.Call(F::kRead, {fd, R::kOutput, 1}) == -9);
    r.Text("host:data.bd");
    CHECK(r.Call(F::kOpen, {R::kPath, 1}) == -19);
    r.Text("short.bh");
    CHECK(r.Call(F::kOpen, {R::kPath, 1}) == -5);
    r.Text("/");
    CHECK(r.Call(F::kOpen, {R::kPath, 1}) < 0);
    CHECK(r.Call(F::kOpen, {0, 1}) < 0);
    std::fill_n(r.ram.begin() + R::kPath, F::kMaxPath, 'x');
    CHECK(r.Call(F::kOpen, {R::kPath, 1}) < 0);
    CHECK(r.Call(F::kOpen, {0xfffffffc, 1}) < 0);
    r.Text("data.bd");
    // Full capacity preserves every outstanding descriptor, unlike OLD's
    // emergency clearing of the entire fake-descriptor table.
    for (uint32_t i = 0; i < F::kMaxFiles; ++i) CHECK(r.Call(F::kOpen, {R::kPath, 1}) == int32_t(i));
    CHECK(r.Call(F::kOpen, {R::kPath, 1}) < 0);
    for (uint32_t i = 0; i < F::kMaxFiles; ++i) {
        CHECK(r.Call(F::kRead, {i, R::kOutput, 3}) == 3);
        CHECK(r.Call(F::kClose, {i}) == 0);
    }
    for (unsigned i = 0; i < 1100; ++i) {
        CHECK(r.Call(F::kOpen, {R::kPath, 1}) == 0);
        CHECK(r.Call(F::kClose, {0}) == 0);
    }
    CHECK(r.files.Snapshot().files.empty() && r.model_calls == 0);
}

void Persistence(const std::string& path) {
    R r(path, true);
    CHECK(r.ready);
    r.Text("data.bd");
    const auto fd = uint32_t(r.Call(F::kOpen, {R::kPath, 1}));
    CHECK(r.Call(F::kRead, {fd, R::kOutput, 2077}) == 2077);
    const auto generation = r.files.Generation(fd);
    const auto saved = r.files.Snapshot();
    std::vector<uint8_t> bytes, captured;
    CHECK(r.files.Encode(bytes));
    const auto read_count = r.files.Count().reads;
    F::State decoded;
    CHECK(r.files.Decode(bytes, decoded) && r.files.Count().reads == read_count);
    for (size_t size = 0; size < bytes.size(); ++size) {
        CHECK(!r.files.Decode(std::vector<uint8_t>(bytes.begin(), bytes.begin() + size), decoded));
        CHECK(r.files.Encode(captured) && captured == bytes);
    }
    auto invalid = saved;
    invalid.files[0].position = invalid.files[0].size + 1;
    CHECK(!r.files.Restore(invalid) && r.files.Generation(fd) == generation);
    invalid = saved; invalid.files.push_back(saved.files[0]);
    CHECK(!r.files.Restore(invalid));
    invalid = saved; ++invalid.files[0].location;
    CHECK(!r.files.Restore(invalid));
    invalid = saved; ++invalid.image_bytes;
    CHECK(!r.files.Restore(invalid));
    CHECK(r.Call(F::kReset) == 0 && r.files.Snapshot().files.empty());
    CHECK(r.Call(F::kRead, {fd, R::kOutput, 1}) == -9);
    CHECK(r.files.Restore(saved) && r.files.Generation(fd) > generation);
    CHECK(r.Call(F::kSeek, {fd, 0, 1}) == 2077);
    CHECK(r.Call(F::kRead, {fd, R::kOutput, 5000}) == int32_t(stream_fixture::kSize - 2077));
    for (uint32_t i = 2077; i < stream_fixture::kSize; ++i)
        CHECK(r.ram[R::kOutput + i - 2077] == stream_fixture::Byte(i));
    // Losing the backing medium is an error with no partial publication.
    CHECK(r.Call(F::kSeek, {fd, 0, 0}) == 0);
    const auto before = r.ram;
    r.iso.Close();
    CHECK(r.Call(F::kRead, {fd, R::kOutput, 64}) == -5 && r.ram == before);
    CHECK(r.Call(F::kSeek, {fd, 0, 1}) == 0);
    CHECK(!r.files.Restore(saved));
}
}

int main() {
    const auto path = stream_fixture::Disc();
    CHECK(!path.empty());
    Compare(path); Errors(path); Persistence(path);
    unlink(path.c_str());
    std::printf("Files: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
