#include <array>
#include <cstdio>
#include <map>
#include <vector>

#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_controls.h"
#include "games/slus_20909_crash_twinsanity/tests/services/control_state_case.h"

static int failures = 0, checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL %d %s\n", __LINE__, #value); } } while (0)

int main() {
    static MainMemory ram;
    static Scratchpad spad;
    static AddressMap map(ram, spad);
    static MmioSink sink;
    static MmioRouter mmio(sink);
    static FastMem fast(ram, map, mmio);
    RecompAbi::BindMemory(&fast);
    CpuContext ctx{};
    constexpr uint32_t gp = 0x3114f0;
    std::map<uint32_t, StreamLibrary::Body> bodies;
    constexpr std::array<uint32_t, 18> entries{0x1e18e8, 0x1e3e80, 0x1e6b50, 0x1e4038,
        0x1e6d30, 0x1e2958, 0x1e1f48, 0x1e3688, 0x1e7cb8, 0x1e7d38, 0x1e6dd0,
        0x1e6e18, 0x1e6e60, 0x1e1500, 0x1e30d0, 0x1e13b0, 0x1e7dd0, 0x1e7d50};
    // Transport faults/callback reentry are explicit fixture inputs here.
    // The independent StreamFileSelfOracle executes the original EE/IOP
    // command bodies and verifies the actual PCM, not these fixture bodies.
    for (auto entry : entries) bodies[entry] = [](CpuContext& call, uint32_t) {
        call.cpuRegs.GPR.r[2].SD[0] = 0;
    };
    auto call = [&](uint32_t entry, std::initializer_list<uint32_t> arguments) {
        ctx.cpuRegs.GPR.r[28].UD[0] = gp;
        ctx.cpuRegs.GPR.r[29].UD[0] = 0x700000;
        unsigned reg = 4;
        for (auto argument : arguments) ctx.cpuRegs.GPR.r[reg++].UD[0] = argument;
        bodies.at(entry)(ctx, 0);
        return ctx.cpuRegs.GPR.r[2].SL[0];
    };
    bodies[0x1e7dd0] = [](CpuContext& c, uint32_t) {
        const auto channel = c.cpuRegs.GPR.r[4].UL[0];
        const auto left = c.cpuRegs.GPR.r[5].UL[0], right = c.cpuRegs.GPR.r[6].UL[0];
        memory::write<uint16_t>(0x2e7a08 + 2 * channel, left);
        memory::write<uint16_t>(0x2e7a68 + 2 * channel, right);
        memory::write<uint32_t>(0x30a554, left);
        memory::write<uint32_t>(0x30a558, right);
    };
    bool busy = false, reenter = false;
    unsigned sent = 0;
    bodies[0x1e2958] = [&](CpuContext& c, uint32_t) {
        ++sent;
        if (reenter) {
            reenter = false;
            const auto saved = c;
            CHECK(call(0x1e6b50, {0, 0x777}) == 0);
            c = saved;
        }
        c.cpuRegs.GPR.r[2].SD[0] = busy ? -1 : 4;
    };
    bool stream_fails = false;
    bodies[0x1e1500] = [&](CpuContext& c, uint32_t) { c.cpuRegs.GPR.r[2].SD[0] = stream_fails ? -1 : 0; };
    auto capture = [&](uint32_t entry) { return bodies.at(entry); };
    auto install = [&](uint32_t entry, StreamLibrary::Body body) { bodies[entry] = std::move(body); return true; };
    title::Audio audio;
    unsigned remaining = 0;
    auto volumes = [&](uint32_t, uint16_t, uint16_t) { ++remaining; };
    CHECK(!title::SoundControls::Install(capture, install, &audio));
    // Partial installation must restore the old entry bodies.
    auto failed_install = [&](uint32_t entry, StreamLibrary::Body body) {
        if (entry == 0x1e6b50) return false;
        bodies[entry] = std::move(body);
        return true;
    };
    CHECK(!title::SoundControls::Install(capture, failed_install, &audio, volumes));
    CHECK(call(0x1e18e8, {}) == 0 && title::SoundControls::Count().staged == 0);
    CHECK(title::SoundControls::Install(capture, install, &audio, volumes));
    audio.ReserveSounds(2);
    std::vector<uint8_t> bytes(16, 0x17);
    bytes[0] = 0; bytes[1] = 7;
    CHECK(audio.StoreSound(0x8000, bytes) && audio.SoundBankLoaded(0x8000));
    CHECK(call(0x1e18e8, {0x8000, 0, 0x1800, 0x2800, 0x1000, 0, 7, 0}) == 0);
    CHECK(!audio.OwnsVoice(0) && memory::read<uint8_t>(0x2e79a8) == 3);
    CHECK(!title::SoundControls::Install(capture, install, nullptr));
    CHECK(audio.ReleaseSound(0x8000)); // queued play must keep its lease
    busy = true;
    CHECK(call(0x1e2958, {1}) == -1 && !audio.OwnsVoice(0));
    busy = false; reenter = true;
    CHECK(call(0x1e2958, {0}) == 14);
    CHECK(audio.OwnsVoice(0) && audio.Inspect().voices[0].player.Capture().parameters.pitch == 0x1000);
    CHECK(!audio.HasSound(0x8000) && audio.IsVoiceFree(0) == 0);
    CHECK(call(0x1e2958, {0}) == 8);
    CHECK(audio.Inspect().voices[0].player.Capture().parameters.pitch == 0x777);
    CHECK(!title::SoundControls::CanCheckpoint());
    const auto boundary_count = title::SoundControls::Count().boundaries;
    CHECK(title::SoundControls::AdvanceBoundary(9) && title::SoundControls::AdvanceBoundary(9));
    CHECK(!title::SoundControls::AdvanceBoundary(8));
    CHECK(title::SoundControls::Count().boundaries == boundary_count + 1);
    // Every command in a large batch survives the source 0x400-word limit.
    const auto volume_count = title::SoundControls::Count().volume;
    const auto sends_before = sent;
    for (uint32_t i = 0; i < 250; ++i) CHECK(call(0x1e3e80, {0, i, i * 2}) == 0);
    CHECK(sent > sends_before);
    CHECK(call(0x1e2958, {0}) >= 0);
    CHECK(title::SoundControls::Count().volume == volume_count + 250);
    CHECK(audio.Inspect().voices[0].left == 249 && audio.Inspect().voices[0].right == 498);
    CHECK(call(0x1e4038, {0, 32}) == -1);
    CHECK(call(0x1e4038, {0, 0}) == 0 && call(0x1e2958, {0}) >= 0);
    CHECK(audio.IsVoiceFree(0) == 0); // release rate is not key-off
    // Status replies cannot overwrite an owned channel with the retained
    // channel's state. Completion is republished only at service replies.
    memory::write<uint8_t>(0x2e79a8, 0);
    call(0x1e1f48, {});
    CHECK(memory::read<uint8_t>(0x2e79a8) == 1);
    // A file load's 0x7e/0x7f transfer does not steal channel zero.
    call(0x1e1500, {3, 0, 0, 0, 0, 0, 0x7e});
    call(0x1e2958, {0});
    CHECK(audio.OwnsVoice(0));
    stream_fails = true;
    call(0x1e1500, {3, 0, 0, 0, 0, 0, 0});
    call(0x1e2958, {0});
    CHECK(audio.OwnsVoice(0));
    stream_fails = false;
    call(0x1e1500, {3, 0, 0, 0, 0, 0, 0});
    CHECK(audio.OwnsVoice(0)); // handoff waits for the same accepted batch
    call(0x1e2958, {0});
    CHECK(!audio.OwnsVoice(0));
    // ResetSound stops voices but is not a bank unload/reset operation.
    CHECK(audio.StoreSound(9, bytes) && audio.SoundBankLoaded(9));
    CHECK(call(0x1e18e8, {9, 0, 3000, 4000, 0x1000, 0, 0, 0}) == 0);
    CHECK(call(0x1e2958, {0}) >= 0);
    call(0x1e6e60, {});
    call(0x1e2958, {0});
    CHECK(!audio.OwnsVoice(0) && audio.HasSound(9));
    // Reset cancels pending owner leases; no stale play can arrive afterwards.
    CHECK(call(0x1e18e8, {9, 0, 3000, 4000, 0x1000, 0, 0, 0}) == 0);
    CHECK(title::SoundControls::Reset());
    CHECK(call(0x1e2958, {0}) == 4 && !audio.OwnsVoice(0));
    // Buffer operations share the accepted prefix, including failure and
    // callback reentry. Movie's ownership barrier must not consume a newly
    // enqueued callback batch in an unbounded recursive send.
    std::vector<title::BufferRequest> accepted;
    bool enqueue_buffer = true;
    CHECK(title::SoundControls::BindBuffers([&](const title::BufferRequest& request) {
        accepted.push_back(request);
        if (enqueue_buffer) {
            enqueue_buffer = false;
            title::BufferRequest next;
            next.kind = title::BufferRequest::Kind::Close; next.stream = 2;
            CHECK(title::SoundControls::StageBuffer(ctx, next, 3));
            CHECK(!title::SoundControls::Flush(ctx));
        }
    }));
    title::BufferRequest allocate;
    allocate.stream = 2; allocate.size = 0x40000; allocate.sound_address = 0x18000;
    CHECK(title::SoundControls::StageBuffer(ctx, allocate, 7));
    allocate.size = 123; // the queue owns the submitted value
    CHECK(!title::SoundControls::BindBuffers({}));
    busy = true;
    CHECK(!title::SoundControls::Flush(ctx) && accepted.empty());
    busy = false;
    ctx.cpuRegs.GPR.r[4].UD[0] = 0x12345678;
    CHECK(!title::SoundControls::Flush(ctx) && accepted.size() == 1 &&
        accepted[0].size == 0x40000 && accepted[0].stream == 2 &&
        ctx.cpuRegs.GPR.r[4].UD[0] == 0x12345678);
    CHECK(title::SoundControls::Flush(ctx) && accepted.size() == 2 &&
        accepted[1].kind == title::BufferRequest::Kind::Close);
    CHECK(title::SoundControls::BindBuffers({}));
    failures += RunControlStateCase(ctx, audio, call, busy);
    CHECK(title::SoundControls::Install(capture, install, nullptr));
    CHECK(title::SoundControls::CanCheckpoint() && remaining == 0);
    std::printf("SoundControls: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
