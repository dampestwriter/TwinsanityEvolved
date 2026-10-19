#include <cstdio>
#include <map>
#include <unistd.h>

#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/core/config/config.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/bank_state.h"
#include "games/slus_20909_crash_twinsanity/hle/audio/sound_banks.h"
#include "tests/cdvd/stream_library_fixture.h"

using namespace stream_fixture;
namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)
}
int main() {
    MainMemory ram;
    Scratchpad spad;
    AddressMap map(ram, spad);
    MmioSink sink;
    MmioRouter mmio(sink);
    FastMem fast(ram, map, mmio);
    RecompAbi::BindMemory(&fast);
    std::map<uint32_t, StreamLibrary::Body> dispatch;
    uint64_t retained = 0;
    auto capture = [&](uint32_t address) {
        if (!dispatch.count(address))
            dispatch[address] = [&](CpuContext& ctx, uint32_t) { ++retained; ctx.cpuRegs.GPR.r[2].SD[0] = 0; };
        return dispatch.at(address);
    };
    auto install = [&](uint32_t address, StreamLibrary::Body body) {
        dispatch[address] = std::move(body); return true;
    };
    CHECK(StreamLibrary::Install(capture, install));
    const auto path = Disc();
    IsoFs iso;
    CHECK(iso.Open(path));
    ReadEngine reader(iso);
    ps2r::CdvdConfig cfg;
    cfg.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    DeliveryMeter meter(cfg);
    DeliveryBudget budget(4096);
    LoaderModel model(meter, budget);
    uint64_t tick = 10;
    uint64_t parks = 0;
    model.SetNowTick([&] { return tick; });
    LoaderFileIo::Deps deps;
    deps.engine = &reader; deps.model = &model; deps.now_tick = [&] { return tick; };
    deps.park_frame = [&] { ++parks; ++tick; };
    LoaderFileIo::Layout layout;
    LoaderFileIo::Bind(deps, layout);
    title::Audio audio;
    CHECK(title::SoundBanks::Install(capture, install, &audio));
    CpuContext ctx{};
    ctx.cpuRegs.GPR.r[28].UD[0] = 0x400000;
    memory::write<int32_t>(0x400000 + layout.counter_gp_offset, 100);
    Text(kPath, "data.bd");
    ctx.cpuRegs.GPR.r[4].UD[0] = kPath;
    LoaderFileIo::Open(ctx);
    const auto handle = ctx.cpuRegs.GPR.r[2].SL[0];
    CHECK(handle == 100);
    ctx.cpuRegs.GPR.r[4].UD[0] = 8;
    dispatch[0x1e7540](ctx, 0x1e7540);
    auto start = [&](uint32_t fs, uint32_t sound, uint32_t offset) {
        Fill(fs, 0x48, 0);
        memory::write<int32_t>(fs + LoaderFileIo::kFsHandle, handle);
        memory::write<uint32_t>(fs + LoaderFileIo::kFsFileSize, kSize);
        ctx.cpuRegs.GPR.r[4].UD[0] = fs; ctx.cpuRegs.GPR.r[5].UD[0] = sound;
        ctx.cpuRegs.GPR.r[6].UD[0] = offset; ctx.cpuRegs.GPR.r[7].UD[0] = 512;
        dispatch[kBank](ctx, kBank);
        CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == 0);
    };
    auto* schedule = StreamLibrary::Scheduling();
    start(kFs, 9, 23);
    schedule->AcceptSounds(schedule->LastOrder());
    start(kFs2, 10, 2051); // queued, deliberately not yet sent
    title::BankState::State saved;
    CHECK(title::BankState::Capture(audio, saved));
    CHECK(saved.files.streams.schedule.reads.at(kFs).accepted);
    CHECK(!saved.files.streams.schedule.reads.at(kFs2).accepted);
    const auto old = saved.audio;
    const auto accepted = saved.files.streams.schedule.reads.at(kFs);
    const title::Audio::LoadTicket old_ticket{9, old.load_generation, old.pending_loads.at(9)};
    std::vector<uint8_t> encoded, file_only;
    CHECK(title::BankState::Encode(audio, saved, encoded));
    CHECK(StreamState::Encode(saved.files, file_only));
    CHECK(!StreamState::Restore(file_only)); // cannot orphan Audio tickets
    title::BankState::State decoded;
    CHECK(title::BankState::Decode(audio, encoded, decoded));
    CHECK(decoded.files.streams.schedule.reads.at(kFs).due == accepted.due);
    CHECK(!decoded.files.streams.schedule.reads.at(kFs2).accepted);
    auto invalid = decoded;
    invalid.audio.pending_loads.emplace(11, old.load_sequence);
    CHECK(!title::BankState::Restore(audio, invalid));
    CHECK(audio.CaptureLoads().load_generation == old.load_generation);
    invalid = decoded;
    invalid.files.streams.schedule.reads.at(kFs).phase = FileSchedule::Phase::Delivered;
    CHECK(!title::BankState::Validate(audio, invalid));
    const auto before = title::SoundBanks::Count();
    ++tick;
    CHECK(!title::BankState::Restore(audio, decoded));
    CHECK(title::SoundBanks::Count().cancelled == before.cancelled);
    tick = saved.files.streams.schedule.tick;
    CHECK(title::BankState::Restore(audio, decoded));
    const auto renewed = audio.CaptureLoads();
    CHECK(renewed.pending_loads == old.pending_loads && renewed.load_generation > old.load_generation);
    CHECK(title::SoundBanks::Count().cancelled == before.cancelled + 2);
    CHECK(!audio.CancelSoundLoad(old_ticket));
    CHECK(!audio.CompleteSoundLoad(old_ticket, std::vector<uint8_t>(512)));
    FileSchedule::State position;
    CHECK(schedule->Capture(position));
    CHECK(position.timing.busy_ms == saved.files.streams.schedule.timing.busy_ms);
    CHECK(position.timing.head_lba == saved.files.streams.schedule.timing.head_lba);
    CHECK(position.budget.remaining == saved.files.streams.schedule.budget.remaining);
    CHECK(position.reads.at(kFs).due == accepted.due && position.reads.at(kFs).order == accepted.order);
    CHECK(!position.reads.at(kFs2).accepted);
    CHECK(!audio.HasSound(9) && !audio.HasSound(10));
    CHECK(schedule->Wait(kFs));
    CHECK(audio.HasSound(9) && !audio.HasSound(10));
    CHECK(audio.Inspect().samples.at(9).ready == false);
    ctx.cpuRegs.GPR.r[4].UD[0] = 9;
    dispatch[0x1e8278](ctx, 0x1e8278);
    CHECK(audio.Inspect().samples.at(9).ready);
    const auto& bytes = *audio.Inspect().samples.at(9).bytes;
    for (uint32_t i = 0; i < bytes.size(); ++i) CHECK(bytes[i] == Byte(i + 23));
    CHECK(schedule->Wait(kFs2) && audio.HasSound(10));
    CHECK(parks == 0 && tick == 10 && model.Enqueued() == 0 && retained == 0);
    // Data-before-done and exactly once survive repeated loads of the same
    // checkpoint; neither an old generation nor the discarded queue can win.
    const auto after_delivery = title::SoundBanks::Count().delivered;
    CHECK(title::BankState::Restore(audio, decoded));
    CHECK(schedule->Wait(kFs) && schedule->Wait(kFs2));
    CHECK(title::SoundBanks::Count().delivered == after_delivery + 2);
    CHECK(audio.CaptureLoads().load_generation > renewed.load_generation);
    CHECK(!audio.CancelSoundLoad({9, renewed.load_generation, old_ticket.sequence}));
    CHECK(title::BankState::Capture(audio, saved)); // delivered assets + file records
    CHECK(title::BankState::Encode(audio, saved, encoded));
    auto corrupt = encoded;
    corrupt.push_back(0);
    CHECK(!title::BankState::Decode(audio, corrupt, decoded));
    for (size_t n : {size_t{0}, size_t{7}, encoded.size() / 2, encoded.size() - 1}) {
        corrupt.assign(encoded.begin(), encoded.begin() + n);
        CHECK(!title::BankState::Decode(audio, corrupt, decoded));
    }
    // Free/replacement invalidates a ticket without erasing its place in the
    // file schedule. Restoring must preserve that stale delivery, not revive
    // a freed sample or let an older read win over its replacement.
    start(kFs, 11, 64);
    CHECK(audio.ReleaseSound(11));
    CHECK(title::BankState::Capture(audio, saved));
    CHECK(title::BankState::Restore(audio, saved));
    auto stale_before = title::SoundBanks::Count().stale;
    CHECK(schedule->Wait(kFs) && !audio.HasSound(11));
    CHECK(title::SoundBanks::Count().stale == stale_before + 1);
    start(kFs, 11, 64);
    start(kFs2, 11, 4096);
    CHECK(title::BankState::Capture(audio, saved));
    CHECK(saved.audio.pending_loads.size() == 1);
    CHECK(title::BankState::Restore(audio, saved));
    stale_before = title::SoundBanks::Count().stale;
    CHECK(schedule->Wait(kFs) && !audio.HasSound(11));
    CHECK(schedule->Wait(kFs2) && audio.HasSound(11));
    CHECK(title::SoundBanks::Count().stale == stale_before + 1);
    const auto& replacement = *audio.Inspect().samples.at(11).bytes;
    for (uint32_t i = 0; i < replacement.size(); ++i) CHECK(replacement[i] == Byte(i + 4096));
    // Legacy v2 had no owned service-time position. Remove that v4 field
    // from the fixed empty-schedule tail, then use the old version word.
    LoaderFileIo::State empty;
    CHECK(LoaderFileIo::CaptureState(empty));
    empty.streams.schedule.reads.clear(); empty.streams.schedule.arrivals.clear();
    empty.streams.schedule.note = FileSchedule::PollOutcome::None;
    empty.streams.schedule.noted_object = 0;
    CHECK(StreamState::Encode(empty, file_only));
    constexpr size_t service_tail = 84, after_service_tail = 64;
    file_only.erase(file_only.end() - service_tail, file_only.end() - after_service_tail);
    file_only[0] = 2;
    CHECK(StreamState::Validate(file_only));
    LoaderFileIo::ResetState();
    title::SoundBanks::Reset();
    CHECK(title::SoundBanks::Install(capture, install, nullptr));
    RecompAbi::BindMemory(nullptr);
    unlink(path.c_str());
    std::printf("BankState: %d checks, %d failures, model enqueues=%llu retained calls=%llu\n",
                checks, failures, static_cast<unsigned long long>(model.Enqueued()),
                static_cast<unsigned long long>(retained));
    return failures ? 1 : 0;
}
