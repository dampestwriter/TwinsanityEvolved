#include <cstdio>
#include <functional>
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
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"
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
    LoaderFileIo::ResetForTest();
    std::map<uint32_t, StreamLibrary::Body> dispatch;
    uint64_t retained = 0;
    const auto capture = [&](uint32_t address) {
        if (!dispatch.count(address))
            dispatch[address] = [&](CpuContext&, uint32_t) { ++retained; };
        return dispatch.at(address);
    };
    const auto install = [&](uint32_t address, StreamLibrary::Body body) {
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
    DeliveryBudget budget(0);
    LoaderModel model(meter, budget);
    uint64_t tick = 100;
    bool advance_tick = true;
    model.SetNowTick([&] { return tick; });
    std::function<void()> on_park;
    LoaderFileIo::Deps deps;
    deps.engine = &reader; deps.model = &model; deps.now_tick = [&] { return tick; };
    deps.park_frame = [&] {
        if (advance_tick) ++tick;
        auto callback = std::move(on_park);
        on_park = {};
        if (callback) callback();
    };
    LoaderFileIo::Layout layout;
    LoaderFileIo::Bind(deps, layout);
    CpuContext ctx{};
    constexpr uint32_t gp = 0x400000;
    ctx.cpuRegs.GPR.r[28].UD[0] = gp;
    const auto open = [&](int32_t wanted) {
        memory::write<int32_t>(gp + layout.counter_gp_offset, wanted);
        Text(kPath, "data.bd");
        ctx.cpuRegs.GPR.r[4].UD[0] = kPath;
        LoaderFileIo::Open(ctx);
        return ctx.cpuRegs.GPR.r[2].SL[0];
    };
    const auto start = [&](CpuContext& context, uint32_t offset, uint32_t dest) {
        context.cpuRegs.GPR.r[4].UD[0] = kFs; context.cpuRegs.GPR.r[5].UD[0] = offset;
        context.cpuRegs.GPR.r[6].UD[0] = 64; context.cpuRegs.GPR.r[7].UD[0] = dest;
        CHECK(LoaderFileIo::StartRead(context));
        return context.cpuRegs.GPR.r[2].SL[0];
    };
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        const bool reopen = (scenario & 1) != 0, sound = (scenario & 2) != 0;
        advance_tick = scenario < 4;
        LoaderFileIo::ResetState();
        LoaderFileIo::Bind(deps, layout);
        CHECK(open(100) == 100);
        const auto generation = StreamLibrary::FileGeneration(100);
        Fill(kFs, 0x48, 0); Fill(kDest, 1024);
        memory::write<uint32_t>(kFs, 2);
        memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, 100);
        memory::write<uint32_t>(kFs + LoaderFileIo::kFsFileSize, kSize);
        meter.ChargeReadAtMs(DeliveryMeter::FrameEndMs(tick), 20, 100);
        CHECK(start(ctx, 128, kDest) == 0);
        const auto issued = StreamLibrary::Scheduling()->Count().issued;
        const auto parks = StreamLibrary::Scheduling()->Count().wait_parks;
        unsigned callbacks = 0;
        on_park = [&] {
            ++callbacks;
            CpuContext callback{};
            callback.cpuRegs.GPR.r[4].UD[0] = 100;
            LoaderFileIo::Close(callback);
            if (reopen) {
                // The same numeric handle and media extent are a new owner.
                // Source restore can reuse numbers, so path equality alone
                // does not establish an operation's lifetime.
                CHECK(open(100) == 100);
                CHECK(StreamLibrary::FileGeneration(100) > generation);
                callback.cpuRegs.GPR.r[28].UD[0] = gp;
                CHECK(start(callback, 512, kDest + 512) == 0);
            }
        };
        if (sound) {
            ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 9;
            ctx.cpuRegs.GPR.r[6].UD[0] = 256; ctx.cpuRegs.GPR.r[7].UD[0] = 64;
            CHECK(!LoaderFileIo::StartSoundRead(ctx, 1, 1));
            CHECK(ctx.cpuRegs.GPR.r[2].SL[0] == -1);
        } else CHECK(start(ctx, 256, kDest + 256) == -1);
        CHECK(callbacks == 1 && retained == 0 && model.Enqueued() == 0);
        CHECK(StreamLibrary::Scheduling()->Count().wait_parks == parks + (advance_tick ? 1 : 0));
        CHECK(memory::read<uint8_t>(kDest) == 0xce &&
              memory::read<uint8_t>(kDest + 256) == 0xce);
        CHECK(StreamLibrary::Scheduling()->Count().issued == issued + (reopen ? 1 : 0));
        if (reopen) {
            CHECK(memory::read<uint32_t>(kFs + LoaderFileIo::kFsFlags) & LoaderFileIo::kFlagReadPending);
            CHECK(StreamLibrary::Scheduling()->InFlight(kFs));
            ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
            LoaderFileIo::WaitReadComplete(ctx);
            CHECK(Bytes(kDest + 512, 512, 64));
            CHECK(!(memory::read<uint32_t>(kFs + LoaderFileIo::kFsFlags) & LoaderFileIo::kFlagReadPending));
            LoaderFileIo::State snapshot;
            CHECK(LoaderFileIo::CaptureState(snapshot));
            const auto before_restore = StreamLibrary::FileGeneration(100);
            CHECK(LoaderFileIo::RestoreState(snapshot));
            CHECK(StreamLibrary::FileGeneration(100) > before_restore);
        } else {
            CHECK(!StreamLibrary::FileOwned(100) && !StreamLibrary::Scheduling()->InFlight(kFs));
        }
    }
    for (bool reopen : {false, true}) {
        LoaderFileIo::ResetState();
        LoaderFileIo::Bind(deps, layout);
        CHECK(open(100) == 100);
        Fill(kFs, 0x48, 0); Fill(kDest, 1024);
        memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, 100);
        memory::write<uint32_t>(kFs + LoaderFileIo::kFsFileSize, kSize);
        const auto generation = StreamLibrary::FileGeneration(100);
        unsigned cancelled = 0, delivered = 0;
        CHECK(StreamLibrary::BindSounds([&](const FileSchedule::Payload&) { ++delivered; },
            [&](const FileSchedule::Payload&) {
                ++cancelled;
                // Cancellation observes a closed descriptor. A callback can
                // create a new incarnation, and this close may not erase it.
                CHECK(!StreamLibrary::FileOwned(100));
                CHECK(!StreamLibrary::CanCheckpoint());
                CHECK(!StreamLibrary::BindSounds({}, {}));
                if (reopen) {
                    CHECK(open(100) == 100);
                    CHECK(StreamLibrary::FileGeneration(100) > generation);
                    CHECK(start(ctx, 512, kDest + 512) == 0);
                }
            }));
        ctx.cpuRegs.GPR.r[4].UD[0] = kFs; ctx.cpuRegs.GPR.r[5].UD[0] = 7;
        ctx.cpuRegs.GPR.r[6].UD[0] = 128; ctx.cpuRegs.GPR.r[7].UD[0] = 64;
        CHECK(LoaderFileIo::StartSoundRead(ctx, 1, 1));
        ctx.cpuRegs.GPR.r[4].UD[0] = 100;
        LoaderFileIo::Close(ctx);
        CHECK(cancelled == 1 && delivered == 0);
        if (reopen) {
            CHECK(StreamLibrary::FileOwned(100));
            LoaderFileIo::State snapshot;
            CHECK(LoaderFileIo::CaptureState(snapshot) && snapshot.files.count(100) == 1);
            ctx.cpuRegs.GPR.r[4].UD[0] = kFs;
            LoaderFileIo::WaitReadComplete(ctx);
            CHECK(Bytes(kDest + 512, 512, 64));
        } else CHECK(!StreamLibrary::FileOwned(100));
        CHECK(StreamLibrary::BindSounds({}, {}));
    }
    LoaderFileIo::ResetState();
    unlink(path.c_str());
    std::printf("FileLifetime: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
