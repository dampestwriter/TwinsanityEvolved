#include "games/slus_20909_crash_twinsanity/composition/input_save.h"
#include "app/host/session_clock.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <unistd.h>

using namespace ps2r::app;
using namespace ps2r::savestate;
static unsigned checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); } } while (false)

int main() {
    char directory[] = "/tmp/input_save_clock_XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    MainMemory ram;
    Scratchpad spad;
    EmuDomain domain;
    TickSource source;
    TickSync ticks;
    SessionClock clock(domain, source, ticks, "input-save-clock");
    InputSave owners(InputSave::Checkpoint::kAll, InputSave::RamAccess(ram, spad), {directory},
        [&] { return domain.DomainTick(); }, {});
    SectionRegistry registry;
    CHECK(!owners.Register(registry, &clock) && registry.Count() == 0);
    CHECK(registry.Register(clock.Section()) && owners.Register(registry, &clock));
    std::map<uint32_t, ::title::InputBindings::Body> entries;
    CHECK(owners.Install([&](uint32_t address, auto body) {
        return entries.emplace(address, std::move(body)).second;
    }));
    auto call = [&](uint32_t address, std::initializer_list<uint32_t> args) {
        CpuContext context{};
        unsigned reg = 4;
        for (const auto arg : args) context.cpuRegs.GPR.r[reg++].UL[0] = arg;
        entries.at(address)(context, 0);
        return context.cpuRegs.GPR.r[2].SD[0];
    };
    auto advance = [&] {
        domain.NoteArrival(source.RawCount());
        domain.OnRawTick(source.Tick());
        domain.OnBoundary(source.RawCount());
    };
    auto finish = [&] {
        for (unsigned i = 0; i < 16; ++i) {
            advance();
            if (!call(0x2a6b68, {0, 0, 0x4200})) return true;
        }
        return false;
    };
    std::memcpy(ram.Data() + 0x4000, "BA", 3);
    std::memcpy(ram.Data() + 0x4020, "SLUS-20909", 11);
    std::memcpy(ram.Data() + 0x4060, "Bank0.bin", 10);
    ram.Data()[0x4100] = 17; ram.Data()[0x4101] = 29;
    CHECK(call(0x2c5080, {}) == 0);
    CHECK(call(0x2a8668, {0x4000, 0x4020}) == 1);
    CHECK(call(0x2a8728, {0, 0, 0x4040}) == 1 && finish());
    CHECK(call(0x2a8c28, {0, 0, 0x4040, 0x4060, 0x4100, 2}) == 1 && finish());
    CHECK(call(0x2c27c0, {0, 0, 0x1000}) == 1);
    ::title::Input::Sample sample;
    sample.connected = true; sample.buttons = 0x8001;
    CHECK(owners.Sample(domain.DomainTick(), sample));
    CHECK(call(0x2c3020, {0, 0, 0, 3}) == 1); // analog mode setup, pending
    CHECK(call(0x2a8df0, {0, 0, 0x4040, 0x4060, 0x4400, 2}) == 1);
    std::string error;
    CHECK(owners.CanCheckpoint(&error));
    // Completed boundary clock is later than the last accepted device sample
    // and storage poll. Neither owner has consumed this key yet.
    advance();
    const auto captured_tick = domain.DomainTick();
    const auto captured = registry.Capture();
    CHECK(!clock.Restored() && registry.Validate(captured, &error));
    auto continuation = [&] {
        std::vector<int64_t> trace;
        for (unsigned i = 0; i < 8; ++i) {
            if (i) advance();
            CHECK(owners.Sample(domain.DomainTick(), sample));
            trace.push_back(static_cast<int64_t>(domain.DomainTick()));
            trace.push_back(call(0x2c2c30, {0, 0}));
            trace.push_back(call(0x2a6b68, {0, 0, 0x4200}));
            trace.push_back(ram.Data()[0x4400]); trace.push_back(ram.Data()[0x4401]);
        }
        return trace;
    };
    const auto original = continuation();
    CHECK(original[1] == static_cast<int32_t>(::title::Input::State::Stable));
    CHECK(original[2] == static_cast<int32_t>(::title::SaveStorage::Operation::Read));
    CHECK(ram.Data()[0x4400] == 17 && ram.Data()[0x4401] == 29);
    ram.Data()[0x4400] = ram.Data()[0x4401] = 0; // same restored RAM output
    CHECK(registry.Restore(captured, &error) && clock.Restored());
    CHECK(domain.DomainTick() == captured_tick);
    CHECK(continuation() == original); // outputs AND completion key, not just bytes

    // An already accepted key must not consume changed host input on rewind.
    const auto accepted = registry.Capture();
    const auto accepted_buttons = owners.Current().sample.buttons;
    sample.buttons = 0;
    advance();
    CHECK(owners.Sample(domain.DomainTick(), sample));
    CHECK(registry.Restore(accepted, &error));
    CHECK(!owners.NeedsSample(domain.DomainTick()) && owners.Sample(domain.DomainTick(), sample));
    CHECK(owners.Current().sample.buttons == accepted_buttons);

    // Pure cross-owner clock validation rejects CRC-valid future owner state
    // before any clock rewind, generation renewal or RAM publication.
    for (const auto tag : {::title::InputStateCodec::kTag, ::title::SaveStateCodec::kTag}) {
        auto future = captured;
        auto& blob = *std::find_if(future.begin(), future.end(), [&](const Blob& b) { return b.tag == tag; });
        if (tag == ::title::InputStateCodec::kTag) {
            ::title::Input::Snapshot value;
            CHECK(::title::InputStateCodec::DecodeSection(blob, value, &error));
            value.boundary = captured_tick + 1;
            CHECK(::title::InputStateCodec::Encode(value, blob.payload, &error));
        } else {
            ::title::SaveBindings::Snapshot value;
            CHECK(::title::SaveStateCodec::Decode(blob.payload, value, &error));
            value.storage.last_boundary = captured_tick + 1;
            CHECK(::title::SaveStateCodec::Encode(value, blob.payload, &error));
        }
        const auto before = SerializeStateFile({}, registry.Capture());
        CHECK(!registry.Restore(future, &error) && error.find("later than") != std::string::npos);
        CHECK(before == SerializeStateFile({}, registry.Capture()));
    }
    // Approved historical boot import is an explicit different-clock policy.
    // Removing a clock is NOT admitted by the real F8 image validator.
    auto legacy = captured;
    legacy.erase(std::remove_if(legacy.begin(), legacy.end(), [](const Blob& blob) {
        return blob.tag == SessionClock::kTag;
    }), legacy.end());
    advance(); advance();
    const auto new_tick = domain.DomainTick();
    CHECK(!clock.ValidateImage({}, legacy, error));
    CHECK(registry.Restore(legacy, &error) && !clock.Restored());
    CHECK(domain.DomainTick() == new_tick && owners.NeedsSample(new_tick));
    CHECK(owners.Sample(new_tick, sample));
    CHECK(owners.Current().connection == ::title::Input::State::Busy);
    CHECK(call(0x2a6b68, {0, 0, 0x4200}) == static_cast<int32_t>(::title::SaveStorage::Operation::Checking));
    advance();
    CHECK(owners.Sample(domain.DomainTick(), sample));
    CHECK(owners.Current().connection == ::title::Input::State::Stable);
    CHECK(call(0x2a6b68, {0, 0, 0x4200}) == static_cast<int32_t>(::title::SaveStorage::Operation::Read));
    std::filesystem::remove_all(directory);
    std::printf("InputSaveClock: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
