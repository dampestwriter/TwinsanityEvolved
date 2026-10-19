#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_blocks.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

#include <cstdio>
#include <set>
#include <stdexcept>

namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
uint64_t calls = 0;
uint32_t allocation = 0x80000;
void ObserveGenerated(uint32_t) { ++calls; }
void ExternalCall(uint32_t at, CpuContext& context) {
    if (at != 0x1815c0) throw std::runtime_error("unexpected block oracle callee");
    const auto size = context.cpuRegs.GPR.r[4].UL[0];
    if (size > 0x100000 || allocation > 0x200000 - size)
        throw std::runtime_error("block oracle allocation bound");
    context.cpuRegs.GPR.r[2].UL[0] = allocation;
    allocation += (size + 15) & ~15u;
}
}
namespace {
using B = title::DistortionBlocks;
namespace ram = strip_oracle::memory;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("blocks line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
constexpr uint32_t Runtime = 0x370450, System = 0x40000, Events = 0x20000, EventPool = 0x21000;
bool Reader(uint32_t at, void* out, size_t size) {
    if (at > ram::bytes.size() || size > ram::bytes.size() - at) return false;
    std::memcpy(out, ram::bytes.data() + at, size); return true;
}
CpuContext Context(uint32_t argument = 0) {
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = argument;
    context.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    context.cpuRegs.GPR.r[29].UL[0] = 0x3f0000;
    context.vuRegs.VF[0].w = 1;
    return context;
}
void Run(B& owner, uint32_t boundary, uint32_t argument = 0) {
    auto context = Context(argument);
    owner.Observe(boundary, context, boundary, [boundary](CpuContext& c, uint32_t) {
        strip_oracle::RunGenerated(boundary, c);
    });
}
void Reset() {
    ram::bytes.fill(0); strip_oracle::allocation = 0x80000;
    ram::write<uint32_t>(0x30a4d8, Events);
    ram::write<uint32_t>(0x30a4f4, EventPool);
    ram::write<uint32_t>(0x30a4e0, 0x22000);
    ram::write<uint32_t>(B::kPool, 0x23000);
    ram::write<uint32_t>(0x30a500, 0x24000);
    ram::write<int32_t>(B::kCount, 8);
    for (uint32_t i = 0; i < 300; ++i) ram::write<uint32_t>(B::kSystems + i * 4, System);
    ram::write<int8_t>(System + 0x22, 7);
    ram::write<float>(System + 0x8c, 0);
    ram::write<int16_t>(Runtime + 0x102, 1);
    ram::write<int8_t>(Runtime + 0x1e3, 1);
    ram::write<int32_t>(0x30a4f0, 1);
    for (uint32_t i = 0; i < 384; ++i) ram::write<int16_t>(0x3a0d90 + i * 2, int16_t(i));
}
B::Snapshot Snapshot(B& owner) {
    B::Snapshot state;
    CHECK(owner.CaptureState(state) && B::Validate(state));
    return state;
}
void SameLinks(B& owner) {
    const auto state = Snapshot(owner);
    for (const auto& pair : state.next) {
        const auto tag = ram::read<uint32_t>(pair.first);
        const auto kind = (tag >> 28) & 7;
        CHECK(kind == 2 || kind == 6);
        const auto source = kind == 6 ? 0 : ram::read<uint32_t>(pair.first + 4) & 0x1fffffff;
        CHECK(source == pair.second);
    }
}
void Resize(B& owner, int16_t count) {
    ram::write<int16_t>(Runtime + 0x10c, count);
    ram::write<int16_t>(Runtime + 0x10e, int16_t(count * 12));
    Run(owner, B::kResize, Runtime); SameLinks(owner);
}
void Lifecycle() {
    Reset(); B owner(Reader);
    Run(owner, B::kInitialize, 4); SameLinks(owner);
    CHECK(owner.Counts().initializes == 1 && Snapshot(owner).next.size() == 8);
    Resize(owner, 3);
    CHECK(ram::read<int16_t>(Runtime + 0x100) == 3);
    const auto first = ram::read<uint32_t>(Runtime), second = ram::read<uint32_t>(Runtime + 4);
    Resize(owner, 6); Resize(owner, 8); // exclusive capacity comparison rejects the last grow
    CHECK(ram::read<int16_t>(Runtime + 0x100) == 6);
    Resize(owner, 2);
    CHECK(Snapshot(owner).next.at(first) == second);
    for (unsigned turn = 0; turn < 12; ++turn) { Run(owner, B::kEvents); SameLinks(owner); }
    Resize(owner, 4); Resize(owner, 0);
    CHECK(Snapshot(owner).next.at(first) != 0); // first released chain survives emitter release
    for (unsigned turn = 0; turn < 48; ++turn) { Run(owner, B::kEvents); SameLinks(owner); }
    CHECK(Snapshot(owner).next.at(first) == 0);
    Resize(owner, 3);
    const auto live = Snapshot(owner);
    std::vector<title::DistortionGeometry::Block> expected;
    const auto head = ram::read<uint32_t>(Runtime);
    CHECK(owner.Capture(head, expected) == B::Result::Ok && expected.size() == 3);
    // Drawing never reads the transport header or trailing program word.
    for (const auto& pair : live.next) {
        std::memset(ram::bytes.data() + pair.first, 0xff, 0x20);
        std::memset(ram::bytes.data() + pair.first + 0x1a0, 0xff, 0x10);
    }
    std::vector<title::DistortionGeometry::Block> captured;
    CHECK(owner.Capture(head | 0x80000000, captured) == B::Result::Ok);
    CHECK(captured.size() == expected.size() &&
          std::memcmp(captured.data(), expected.data(), captured.size() * sizeof(captured[0])) == 0);
    B restored(Reader);
    CHECK(restored.Restore(live) && !restored.Restore(live));
    CHECK(restored.Capture(head, captured) == B::Result::Ok && captured.size() == 3);
    auto invalid = live; invalid.next.at(head) = head;
    CHECK(!B::Validate(invalid));
    invalid = live; invalid.next.at(head) = 0x1234;
    CHECK(!B::Validate(invalid));
    CHECK(restored.Capture(0x1234, captured) == B::Result::UnsupportedSource && captured.size() == 3);
}
void EventsAndFailure() {
    Reset(); B owner(Reader); Run(owner, B::kInitialize, 4); Resize(owner, 4);
    // The otherwise unqueued BlockTaken branch still has exact semantics.
    const auto event = ram::read<uint32_t>(EventPool);
    const auto middle = ram::read<uint32_t>(Runtime + 4);
    ram::write<uint32_t>(event, middle); ram::write<uint32_t>(event + 8, 1);
    ram::write<uint32_t>(event + 12, Runtime); ram::write<uint32_t>(event + 16, 0);
    ram::write<uint32_t>(B::kEventWheel, event);
    ram::write<uint32_t>(0x30a4f8, 1); // one taken source event
    Run(owner, B::kEvents); SameLinks(owner);
    CHECK(ram::read<int16_t>(Runtime + 0x100) == 3);
    for (unsigned turn = 0; turn < 8; ++turn) { Run(owner, B::kEvents); SameLinks(owner); }
    CHECK(Snapshot(owner).next.at(middle) == 0);
    auto context = Context();
    bool rejected = false; unsigned originals = 0;
    try { owner.Observe(B::kEvents, context, B::kEvents + 4,
        [&](CpuContext&, uint32_t) { ++originals; }); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected && !originals && owner.Busy());
    B::Snapshot sentinel; sentinel.generation = 99;
    CHECK(!owner.CaptureState(sentinel) && sentinel.generation == 99);
}
void DetachedAndDelayed() {
    Reset(); B owner(Reader); Run(owner, B::kInitialize, 4); Resize(owner, 4);
    const auto head = ram::read<uint32_t>(Runtime);
    const auto before = Snapshot(owner);
    ram::write<int32_t>(0x31000, 0);
    auto context = Context(0x31000);
    strip_oracle::RunGenerated(0x1b47e0, context); // complete source FreeEmitterBlocks
    CHECK(ram::read<int32_t>(0x31000) == -1);
    CHECK(Snapshot(owner).next == before.next); // ownership survives emitter lifetime
    std::vector<title::DistortionGeometry::Block> blocks;
    CHECK(owner.Capture(head, blocks) == B::Result::Ok && blocks.size() == 4);
    SameLinks(owner);
    for (unsigned turn = 0; turn < 8; ++turn) { Run(owner, B::kEvents); SameLinks(owner); }
    CHECK(Snapshot(owner).next.at(head) == 0);

    Reset(); B delayed(Reader); Run(delayed, B::kInitialize, 4); Resize(delayed, 3);
    const auto block = ram::read<uint32_t>(Runtime);
    const auto event = ram::read<uint32_t>(EventPool);
    ram::write<uint32_t>(event, block); ram::write<uint32_t>(event + 4, 33);
    ram::write<uint32_t>(event + 8, 9); ram::write<uint32_t>(event + 16, 0);
    ram::write<uint32_t>(B::kEventWheel, event);
    ram::write<uint32_t>(0x30a4f8, 1);
    for (unsigned turn = 0; turn < 33; ++turn) {
        Run(delayed, B::kEvents); SameLinks(delayed);
        CHECK(Snapshot(delayed).next.at(block) != 0);
    }
    Run(delayed, B::kEvents); SameLinks(delayed);
    CHECK(Snapshot(delayed).next.at(block) == 0);
}
}
int main() {
    try {
        Lifecycle(); EventsAndFailure(); DetachedAndDelayed();
        std::printf("DistortionBlocks: %u checks, %llu complete generated CPU calls; no header reads in owner\n",
            checks, (unsigned long long)strip_oracle::calls);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
}
