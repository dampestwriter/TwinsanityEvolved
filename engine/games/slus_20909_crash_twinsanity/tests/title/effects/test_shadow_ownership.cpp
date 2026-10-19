#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_loads.h"
#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_assets_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_source.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <map>
#include <tuple>

namespace {
using A = title::ShadowAssets;
using L = title::ShadowLoads;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
void Store(std::vector<uint8_t>& memory, uint32_t at, uint32_t value) {
    CHECK(at <= memory.size() && 4 <= memory.size() - at);
    std::memcpy(memory.data() + at, &value, 4);
}
std::vector<uint8_t> Part(float x) {
    std::vector<uint32_t> words{0, 0, 0x070000fa, 0, 0x6c018000, 0x8003,
        0x30024000, 0x512, 0, 0x01000101, 0x64018001, 12, 0x8003, 0x01000104, 0x68038003};
    for (float value : {x, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f}) {
        uint32_t bits; std::memcpy(&bits, &value, 4); words.push_back(bits);
    }
    words.push_back(0x6c038004);
    for (uint32_t restart : {0x8000, 0x8000, 0})
        words.insert(words.end(), {0, 0, 0, restart});
    words.push_back(0x68038005);
    words.insert(words.end(), {0, 0, 0, 0, 0, 0, 32, 0, 0});
    words.insert(words.end(), {0x14000000, 0x01000101});
    while (words.size() % 4) words.push_back(0);
    words[0] = 0x60000000 | uint32_t(words.size() / 4 - 1);
    std::vector<uint8_t> bytes(words.size() * 4);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}
void Seed(std::vector<uint8_t>& memory, float x = 0) {
    const auto bytes = Part(x);
    Store(memory, 0x1004, 0xd8d37a1c);
    Store(memory, 0x1008, 1);
    Store(memory, 0x100c, 0x2000);
    Store(memory, 0x1010, 0x2004);
    Store(memory, 0x1018, 0x2008);
    Store(memory, 0x2000, 3);
    Store(memory, 0x2004, uint32_t(bytes.size()));
    Store(memory, 0x2008, 7);
    Store(memory, A::kDiskManager + 0x58 + 7 * 4, 0x3000);
    Store(memory, 0x3000, 7);
    Store(memory, 0x3004, 0x4000);
    Store(memory, 0x3008, uint32_t(bytes.size()));
    Store(memory, 0x301c, 1 | (1 << 14));
    std::memcpy(memory.data() + 0x4000, bytes.data(), bytes.size());
}
void Lifetime() {
    std::vector<uint8_t> memory(0x02000000);
    unsigned reads = 0;
    title::MaterialState::Read read = [&](uint32_t at, void* out, size_t size) {
        ++reads;
        if (at > memory.size() || size > memory.size() - at) return false;
        std::memcpy(out, memory.data() + at, size);
        return true;
    };
    Seed(memory);
    A assets;
    CHECK(assets.ModelLoaded(read, 0x80001000) == A::Result::Ok);
    auto original = assets.Find(0x1000);
    CHECK(original && original->token.generation == 1);
    CHECK(original->mesh.strips[0].vertices[2].reverse_fan);
    CHECK(!assets.Find(0x60001000));
    const auto reads_after_load = reads;
    CHECK(assets.Find(0xa0001000) == original && reads == reads_after_load);
    A::Snapshot sentinel;
    sentinel.version = 45;
    CHECK(assets.Capture(sentinel) == A::Result::RetainedConsumer && sentinel.version == 45);
    Seed(memory, 12);
    CHECK(assets.ModelLoaded(read, 0x1000) == A::Result::Ok);
    auto reloaded = assets.Find(0x1000);
    CHECK(reloaded != original && reloaded->token.generation == 2);
    CHECK(original->mesh.strips[0].vertices[0].position[0] == 0);
    CHECK(reloaded->mesh.strips[0].vertices[0].position[0] == 12);
    CHECK(assets.ResetAfterQuiescence() == A::Result::RetainedConsumer);
    assets.Forget(0xa0001000);
    CHECK(!assets.Find(0x1000) && assets.Current() == 0);
    CHECK(reloaded->mesh.strips[0].vertices[0].position[0] == 12);
    CHECK(assets.Capture(sentinel) == A::Result::RetainedConsumer);
    original.reset(); reloaded.reset();
    CHECK(assets.Capture(sentinel) == A::Result::Ok && sentinel.assets.empty());
    CHECK(sentinel.next_generation == 3 && sentinel.version == 1);
    CHECK(assets.ModelLoaded(read, 0x1000) == A::Result::Ok);
    Store(memory, 0x301c, 0); // not resident: never publish zero-filled data
    CHECK(assets.ModelLoaded(read, 0x1000) == A::Result::InvalidMemory);
    CHECK(!assets.Find(0x1000));
    Seed(memory);
    Store(memory, 0x3004, 0xfffffff0); // source span must not wrap or alias
    CHECK(assets.ModelLoaded(read, 0x1000) == A::Result::InvalidMemory);
    Seed(memory);
    Store(memory, 0x4000 + 16, 0);
    CHECK(assets.ModelLoaded(read, 0x1000) == A::Result::InvalidAsset);
    Seed(memory);
    CHECK(assets.ModelLoaded(read, 0x1000) == A::Result::Ok);
    CHECK(assets.Capture(sentinel) == A::Result::Ok);
    CHECK(A::Validate(sentinel) && A::ValidateSource(sentinel, read));
    Store(memory, 0x301c, 0);
    CHECK(A::ValidateSource(sentinel, read)); // converted data owns its lifetime
    Store(memory, 0x1004, 123);
    CHECK(!A::ValidateSource(sentinel, read));
    CHECK(assets.ModelLoaded(read, 0x1000) == A::Result::NotSelected && !assets.Find(0x1000));
    A restored;
    CHECK(restored.Restore(sentinel) == A::Result::Ok);
    CHECK(restored.Restore(sentinel) == A::Result::OwnerNotEmpty);
    CHECK(restored.Find(0x1000)->token.generation == sentinel.assets[0].token.generation);
    CHECK(restored.Find(0x1000)->bytes == sentinel.assets[0].bytes);
    auto bad = sentinel;
    bad.assets[0].bytes.pop_back();
    A empty;
    CHECK(!A::Validate(bad) && empty.Restore(bad) == A::Result::InvalidSnapshot);
    CHECK(empty.Current() == 0);
    bad = sentinel; bad.next_generation = bad.assets[0].token.generation;
    CHECK(!A::Validate(bad));
    bad = sentinel; bad.assets.push_back(bad.assets[0]);
    CHECK(!A::Validate(bad));
    CHECK(assets.ResetAfterQuiescence() == A::Result::Ok);
    CHECK(assets.Restore(sentinel) == A::Result::OwnerNotEmpty); // generator was used
    auto exhausted = sentinel;
    exhausted.next_generation = 0;
    A no_generations;
    CHECK(no_generations.Restore(exhausted) == A::Result::Ok);
    Seed(memory);
    CHECK(no_generations.ModelLoaded(read, 0x1000) == A::Result::LimitExceeded);
    CHECK(!no_generations.Find(0x1000));
    for (uint32_t id = 0x67; id != 0x6f; ++id) CHECK(A::IsSourceModel(id));
    CHECK(!A::IsSourceModel(0x66) && !A::IsSourceModel(0x6f));
}
void Bindings() {
    std::vector<uint8_t> memory(0x02000000);
    title::MaterialState::Read read = [&](uint32_t at, void* out, size_t size) {
        if (at > memory.size() || size > memory.size() - at) return false;
        std::memcpy(out, memory.data() + at, size); return true;
    };
    A assets;
    L loads(assets, read);
    std::map<uint32_t, L::Body> dispatch;
    unsigned reads = 0, destroys = 0;
    dispatch[A::kRead] = [&](CpuContext& c, uint32_t) {
        ++reads;
        CHECK(!assets.Find(0x1000)); // prior version invalidated before source mutation
        Seed(memory, 7);
        c.cpuRegs.GPR.r[4].UL[0] = 0xdeadbeef; // source may clobber its arguments
    };
    dispatch[A::kDestroy] = [&](CpuContext&, uint32_t) {
        ++destroys;
        CHECK(!assets.Find(0x1000));
    };
    const auto capture = [&](uint32_t address) { return dispatch[address]; };
    const auto install = [&](uint32_t address, L::Body body) {
        dispatch[address] = std::move(body); return true;
    };
    CHECK(loads.Install(capture, install));
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x1000;
    dispatch[A::kRead](context, 0);
    CHECK(reads == 1 && loads.Counts().converted == 1);
    CHECK(assets.Find(0x1000)->mesh.strips[0].vertices[0].position[0] == 7);
    auto queued = assets.Find(0x1000);
    context.cpuRegs.GPR.r[4].UL[0] = 0x1000;
    dispatch[A::kDestroy](context, A::kDestroy);
    CHECK(destroys == 1 && !assets.Find(0x1000));
    CHECK(queued->mesh.strips[0].vertices[0].position[0] == 7);
    bool refused = false;
    try { dispatch[A::kRead](context, A::kRead + 4); } catch (...) { refused = true; }
    CHECK(refused && reads == 1 && loads.Counts().interior == 1);
    A other;
    L rejected(other, read);
    unsigned called = 0, installed = 0;
    std::map<uint32_t, L::Body> rollback;
    rollback[A::kRead] = [&](CpuContext&, uint32_t) { ++called; };
    rollback[A::kDestroy] = rollback[A::kRead];
    CHECK(!rejected.Install([&](uint32_t address) { return rollback[address]; },
        [&](uint32_t address, L::Body body) {
            ++installed;
            rollback[address] = std::move(body);
            if (installed == 2) return false; // rejecting publication still needs undo
            return true;
        }));
    rollback[A::kRead](context, 0);
    rollback[A::kDestroy](context, 0);
    CHECK(installed == 4 && called == 2 && rejected.Counts().reads == 0);
    // A rejected rollback can retain a callback after the owner is destroyed.
    // Its inactive path must remain a safe forwarder to the captured source.
    {
        L failed(other, read);
        installed = 0;
        refused = false;
        try {
            failed.Install([&](uint32_t address) { return rollback[address]; },
                [&](uint32_t address, L::Body body) {
                    ++installed;
                    if (installed > 2) return false;
                    rollback[address] = std::move(body);
                    return installed != 2;
                });
        } catch (...) { refused = true; }
        CHECK(refused && installed == 4);
    }
    rollback[A::kRead](context, 0);
    rollback[A::kDestroy](context, 0);
    CHECK(called == 4);
}
// The all-on frame-113 shape: the model's DiskManager node lives in the META
// arena (hle.meta_arena routing) and the reader serves it -> converted; a node
// the reader cannot serve -> the load is REFUSED (counted, attributed through
// the sink with the Result), never a throw out of the title's load; the model
// stays unowned.
void ArenaNodeAndRefusal() {
    std::vector<uint8_t> memory(0x02000000), arena(MetaArena::kSize);
    title::MaterialState::Read read = [&](uint32_t at, void* out, size_t size) {
        if (at >= MetaArena::kBase) {
            const uint32_t offset = at - MetaArena::kBase;
            if (offset > arena.size() || size > arena.size() - offset) return false;
            std::memcpy(out, arena.data() + offset, size); return true;
        }
        if (at > memory.size() || size > memory.size() - at) return false;
        std::memcpy(out, memory.data() + at, size); return true;
    };
    std::vector<std::tuple<title::GraphicsRefusals::Reason, uint32_t, uint32_t>> refusals;
    A assets;
    L loads(assets, read, [&](auto reason, uint32_t producer, uint32_t detail) {
        refusals.emplace_back(reason, producer, detail);
    });
    std::map<uint32_t, L::Body> dispatch;
    dispatch[A::kRead] = [&](CpuContext&, uint32_t) {};   // the source body: the bytes are already loaded
    dispatch[A::kDestroy] = [&](CpuContext&, uint32_t) {};
    CHECK(loads.Install([&](uint32_t address) { return dispatch[address]; },
                        [&](uint32_t address, L::Body body) { dispatch[address] = std::move(body); return true; }));
    Seed(memory, 3);
    // Node 7 moved into the arena: the handle table points above main RAM, the
    // RAM copy is gone.
    const uint32_t node = MetaArena::kBase + 0x1a8c;
    std::memcpy(arena.data() + 0x1a8c, memory.data() + 0x3000, 0x20);
    std::fill(memory.begin() + 0x3000, memory.begin() + 0x3020, 0);
    Store(memory, A::kDiskManager + 0x58 + 7 * 4, node);
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x1000;
    dispatch[A::kRead](context, 0);
    CHECK(loads.Counts().converted == 1 && loads.Counts().refused == 0 && refusals.empty());
    CHECK(assets.Find(0x1000) && assets.Find(0x1000)->mesh.strips[0].vertices[0].position[0] == 3);
    // The node where no reader serves it: refused by name, the title's load completes.
    Store(memory, A::kDiskManager + 0x58 + 7 * 4, MetaArena::kBase + MetaArena::kSize - 4);
    bool threw = false;
    context.cpuRegs.GPR.r[4].UL[0] = 0x1000;
    try { dispatch[A::kRead](context, 0); } catch (...) { threw = true; }
    CHECK(!threw && loads.Counts().reads == 2 && loads.Counts().refused == 1 && loads.Counts().failed == 0);
    CHECK(!assets.Find(0x1000) && assets.Counts().rejected == 1);
    CHECK(refusals.size() == 1 && std::get<0>(refusals[0]) == title::GraphicsRefusals::Reason::UnsupportedState &&
          std::get<1>(refusals[0]) == A::kRead && std::get<2>(refusals[0]) == uint32_t(A::Result::InvalidMemory));
    CHECK(loads.CanCheckpoint());
}
void Codec() {
    using C = title::ShadowAssetsCodec;
    A::Snapshot snapshot;
    snapshot.next_generation = 8;
    snapshot.assets.push_back({{0x1000, 2}, 0xd8d37a1c, 3, Part(2)});
    snapshot.assets.push_back({{0x2000, 7}, 0x6bb7b765, 3, Part(7)});
    std::vector<uint8_t> bytes, again;
    CHECK(C::Encode(snapshot, bytes));
    A::Snapshot decoded;
    CHECK(C::Decode(bytes, decoded) && C::Encode(decoded, again) && bytes == again);
    CHECK(decoded.next_generation == 8 && decoded.assets.size() == 2);
    CHECK(decoded.assets[0].bytes == Part(2) && decoded.assets[1].bytes == Part(7));
    A::Snapshot sentinel;
    sentinel.version = 55;
    for (size_t size = 0; size < bytes.size(); ++size) {
        CHECK(!C::Decode({bytes.begin(), bytes.begin() + size}, sentinel));
        CHECK(sentinel.version == 55);
    }
    auto corrupt = bytes;
    corrupt.push_back(0);
    CHECK(!C::Decode(corrupt, sentinel) && sentinel.version == 55);
    for (const auto offset : {0u, 4u, 16u, 20u, 24u, 40u}) {
        corrupt = bytes;
        Store(corrupt, offset, 0xffffffff);
        CHECK(!C::Decode(corrupt, sentinel) && sentinel.version == 55);
    }
    snapshot.assets[1].token.generation = snapshot.assets[0].token.generation;
    again = {1, 2, 3};
    CHECK(!C::Encode(snapshot, again) && again == std::vector<uint8_t>({1, 2, 3}));
    CHECK(!C::Decode({}, sentinel) && sentinel.version == 55);
}
void SourceInstance() {
    using S = title::ShadowSource;
    std::vector<uint8_t> memory(0x02000000);
    title::MaterialState::Read read = [&](uint32_t at, void* out, size_t size) {
        if (at > memory.size() || size > memory.size() - at) return false;
        std::memcpy(out, memory.data() + at, size); return true;
    };
    Seed(memory);
    A assets;
    CHECK(assets.ModelLoaded(read, 0x1000) == A::Result::Ok);
    // Source RigidModel -> MaterialResource -> Material -> type-0B shader.
    Store(memory, 0x1108, 0x2100);
    Store(memory, 0x110c, 0x1000);
    Store(memory, 0x1118, 1);
    Store(memory, 0x111c, 0xd8d37a1c);
    Store(memory, 0x2100, 0x2200);
    Store(memory, 0x2208, 0x6000);
    Store(memory, 0x6000, 0x7000);
    Store(memory, 0x6010, 1);
    Store(memory, 0x6018, 32);
    Store(memory, 0x6020, 21);
    Store(memory, 0x7064, 11);
    Store(memory, 0x706c, S::kShaderVtable);
    Store(memory, S::kView, 0x8000);
    const title::ShadowGeometry::Matrix matrix{{
        {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    std::memcpy(memory.data() + 0x9000, matrix.data(), sizeof(matrix));
    std::memcpy(memory.data() + 0x9040, matrix.data(), sizeof(matrix));
    const title::ShadowGeometry::Vector guard{1.1f, 1.2f, 1.3f, 1};
    std::memcpy(memory.data() + 0x8140, guard.data(), sizeof(guard));
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = 0x1100;
    context.cpuRegs.GPR.r[5].UL[0] = 0x9000;
    context.cpuRegs.GPR.r[6].UL[0] = 0x9040;
    S::Instance out;
    CHECK(S::Mesh(read, assets, {4, 512, 448}, context, out) == S::Result::Ok);
    CHECK(out.object == 0x1100 && out.material == 0x6000 && out.asset == assets.Find(0x1000));
    CHECK(out.view.screen == matrix && out.view.clip == matrix && out.view.guard == guard);
    CHECK(out.view.offset[0] == 1920 && out.view.offset[1] == 1936);
    // needsEye is the type-0B shader's answer (NTSC 1d9330 returns 1): accepted,
    // carried as the source instance size (11 quadwords), never a shape refusal.
    CHECK(!out.eye && S::InstanceSize(out) == S::kClippedSize);
    Store(memory, 0x1114, 1);
    CHECK(S::Mesh(read, assets, {4, 512, 448}, context, out) == S::Result::Ok);
    CHECK(out.eye && S::InstanceSize(out) == S::kEyeSize);
    Store(memory, 0x1114, 0);
    // An open instance block, a billboard shader, an open material writer, a
    // foreign bucket and a foreign shader vtable (slot 1 of the real one) refuse.
    for (const auto& change : std::array<std::array<uint32_t, 2>, 5>{{
             {0x1110, 0x5000}, {0x1114, 0x100}, {0x6028, 0x5000},
             {0x6020, 22}, {0x706c, S::kShaderVtable + 8}}}) {
        uint32_t previous;
        CHECK(read(change[0], &previous, 4));
        Store(memory, change[0], change[1]);
        out.object = 55;
        CHECK(S::Mesh(read, assets, {4, 512, 448}, context, out) == S::Result::UnsupportedSource);
        CHECK(out.object == 55);
        Store(memory, change[0], previous);
    }
    assets.Forget(0x1000);
    CHECK(S::Mesh(read, assets, {4, 512, 448}, context, out) == S::Result::UnsupportedSource);
    CHECK(out.asset->mesh.strips[0].vertices[2].reverse_fan); // prior accepted copy remains valid
}
}
int main() {
    try {
        Lifetime();
        ArenaNodeAndRefusal();
        Bindings();
        Codec();
        SourceInstance();
        std::printf("ShadowOwnership: %u checks, load/lifetime/checkpoint/source-binding pins\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
