#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <unistd.h>

#include "games/slus_20909_crash_twinsanity/composition/file_services.h"
#include "games/slus_20909_crash_twinsanity/composition/service_import.h"
#include "games/slus_20909_crash_twinsanity/composition/service_restore.h"
#include "app/host/session_clock.h"
#include "games/slus_20909_crash_twinsanity/composition/session_services.h"
#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/core/config/config.h"
#include "engine/core/verify/memory_checkpointer.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/host_app/memory.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/ee/savestate/thread_section.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "tests/cdvd/stream_library_fixture.h"

using namespace ps2r::app;
using namespace ps2r::savestate;
using namespace stream_fixture;
namespace {
int failures = 0, checks = 0;
std::string error;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::printf("FAIL %d: %s: %s\n", __LINE__, #value, error.c_str()); } } while (false)
int32_t Sdk(uint32_t entry, std::initializer_list<uint32_t> arguments) {
    CpuContext context{};
    unsigned reg = 4;
    for (const auto arg : arguments) context.cpuRegs.GPR.r[reg++].UL[0] = arg;
    DispatchTable::Invoke(entry, context);
    return context.cpuRegs.GPR.r[2].SL[0];
}
void Remove(std::vector<Blob>& image, uint32_t tag) {
    image.erase(std::remove_if(image.begin(), image.end(),
        [tag](const auto& blob) { return blob.tag == tag; }), image.end());
}
Blob& Find(std::vector<Blob>& image, uint32_t tag) {
    return *std::find_if(image.begin(), image.end(), [tag](const auto& blob) { return blob.tag == tag; });
}
}

int main() {
    MainMemory ram;
    Scratchpad scratch;
    AddressMap map(ram, scratch);
    MmioSink sink;
    MmioRouter mmio(sink);
    FastMem fast(ram, map, mmio);
    RecompAbi::BindMemory(&fast);
    ps2r::ipu::GuestMemoryAdapter guest([&](uint32_t at, size_t size) -> uint8_t* {
        return at <= MainMemory::kSize && size <= MainMemory::kSize - at ? ram.Data() + at : nullptr;
    });
    const auto path = Disc();
    IsoFs iso;
    CHECK(iso.Open(path));
    ReadEngine reader(iso);
    ps2r::CdvdConfig config;
    config.delivery_visibility = ps2r::CdvdDeliveryVisibility::Poll;
    DeliveryMeter meter(config);
    DeliveryBudget budget(0);
    LoaderModel model(meter, budget);
    EmuDomain domain;
    domain.SetRegimePin(1);
    TickSource source;
    TickSync ticks;
    const auto advance = [&] {
        domain.NoteArrival(source.RawCount());
        domain.OnRawTick(source.Tick());
        domain.OnBoundary(source.RawCount());
    };
    for (unsigned i = 0; i < 100; ++i) advance();
    SessionClock clock(domain, source, ticks, "SLUS-20909:service-restore-fixture");
    LoaderFileIo::ResetForTest();
    std::map<uint32_t, StreamLibrary::Body> dispatch;
    uint64_t original_calls = 0;
    CHECK(StreamLibrary::Install([&](uint32_t) -> StreamLibrary::Body {
        return [&](CpuContext&, uint32_t) { ++original_calls; };
    }, [&](uint32_t at, StreamLibrary::Body body) { dispatch[at] = std::move(body); return true; }));
    LoaderFileIo::Deps deps;
    deps.engine = &reader; deps.model = &model;
    deps.now_tick = [&] { return domain.DomainTick(); };
    deps.park_frame = advance;
    deps.call = [&](uint32_t at, CpuContext& context) {
        if (at == kCopy) {
            // The title's ring copy-out (cb vtbl+0x14): memcpy(dest, cursor, n); cursor += n.
            const auto cb = context.cpuRegs.GPR.r[4].UL[0], dst = context.cpuRegs.GPR.r[5].UL[0],
                       count = context.cpuRegs.GPR.r[6].UL[0];
            const auto cursor = memory::read<uint32_t>(cb + LoaderFileIo::kCbCursor);
            for (uint32_t i = 0; i < count; ++i) memory::write<uint8_t>(dst + i, memory::read<uint8_t>(cursor + i));
            memory::write<uint32_t>(cb + LoaderFileIo::kCbCursor, cursor + count);
        } else if (dispatch.count(at)) dispatch.at(at)(context, at);
        else ++original_calls;
    };
    LoaderFileIo::Layout layout;
    LoaderFileIo::Bind(deps, layout);
    CHECK(StreamLibrary::Scheduling()->SetServiceTime(true));
    CpuContext context{};
    context.cpuRegs.GPR.r[28].UD[0] = 0x400000;
    memory::write<int32_t>(0x400000 + layout.counter_gp_offset, 100);
    Text(kPath, "data.bd");
    context.cpuRegs.GPR.r[4].UD[0] = kPath;
    LoaderFileIo::Open(context);
    CHECK(context.cpuRegs.GPR.r[2].SL[0] == 100);
    Fill(kFs, 0x48, 0);
    memory::write<uint32_t>(kFs, 2);
    memory::write<int32_t>(kFs + LoaderFileIo::kFsHandle, 100);
    memory::write<uint32_t>(kFs + LoaderFileIo::kFsFileSize, kSize);
    Fill(kDest, 512);
    const auto issue = [&](uint32_t offset, uint32_t destination) {
        context.cpuRegs.GPR.r[4].UD[0] = kFs; context.cpuRegs.GPR.r[5].UD[0] = offset;
        context.cpuRegs.GPR.r[6].UD[0] = 128; context.cpuRegs.GPR.r[7].UD[0] = destination;
        CHECK(LoaderFileIo::StartRead(context));
    };
    meter.ChargeReadAtMs(DeliveryMeter::FrameEndMs(domain.DomainTick()), 20, 100);
    issue(512, kDest);
    CHECK(StreamLibrary::Scheduling()->InFlight(kFs));
    DispatchTable::Initialize();
    FileServices files(guest, iso, true), off_files(guest, iso, false);
    CHECK(files.Install());
    Text(kPath, "cdrom0:\\DATA.BD;1");
    const auto descriptor = uint32_t(Sdk(title::Files::kOpen, {kPath, 1}));
    CHECK(int32_t(descriptor) >= 0);
    CHECK(Sdk(title::Files::kRead, {descriptor, kDest + 256, 13}) == 13);
    ticks.NotePending(TickSync::Cause::Demand);
    ticks.NotePending(TickSync::Cause::Demand);
    MemorySections memory_sections(ram, scratch, nullptr, 0);
    StateRestorer restorer(memory_sections);
    restorer.RequireSectionOwners();
    SessionServices services(true, nullptr), off_services(false, nullptr);
    ServiceRestore join(clock, services, files), off_join(clock, off_services, off_files);
    CHECK(restorer.Registry().Register(clock.Section()));
    CHECK(restorer.Registry().Register(services.Section()));
    CHECK(restorer.Registry().Register(files.Section()));
    CHECK(join.RegisterValidation(restorer.Registry()) && join.CanCheckpoint(&error));
    unsigned pre_restores = 0;
    restorer.SetPreRestore([&](std::string*) { ++pre_restores; return true; });
    restorer.SetImageValidation([&](const auto& meta, const auto& image, std::string* why) {
        return join.ValidateImage(meta, image, true, why);
    });
    FileMeta meta;
    meta.vsync_frame = uint32_t(domain.BoundaryCount());
    const auto saved_tick = domain.DomainTick();
    const auto saved_generation = StreamLibrary::FileGeneration(100);
    auto captured = restorer.Registry().Capture();
    const auto snapshot = SerializeStateFile(meta, captured);
    FileSchedule::State pending;
    CHECK(StreamLibrary::Scheduling()->Capture(pending, &error));
    const auto deadline = pending.reads.at(kFs).end_ms;
    CHECK(join.ValidateImage(meta, captured, false, &error));
    for (unsigned i = 0; i < 20; ++i) advance();
    StreamLibrary::AdvanceBoundary(domain.DomainTick());
    CHECK(Bytes(kDest, 512, 128));
    issue(1024, kDest + 128);
    CHECK(Sdk(title::Files::kSeek, {descriptor, 300, 0}) == 300);
    ticks.NotePending(TickSync::Cause::Demand);
    const auto future_clock = clock.Capture();
    const auto future_services = services.Section();
    std::vector<uint8_t> future_bytes;
    future_services.serialize(future_bytes);
    ram.Data()[0x100] = 0x73;
    const auto reject = [&](const std::vector<Blob>& image, FileMeta wrong_meta) {
        const auto bytes = SerializeStateFile(wrong_meta, image);
        CHECK(!restorer.RestoreImage(bytes.data(), bytes.size(), &error));
        std::vector<uint8_t> after;
        future_services.serialize(after);
        CHECK(pre_restores == 0 && ram.Data()[0x100] == 0x73 && after == future_bytes &&
              domain.DomainTick() == future_clock.domain.tick && ticks.Pending() == future_clock.pending_ticks);
        CHECK(Sdk(title::Files::kSeek, {descriptor, 0, 1}) == 300);
    };
    auto malformed = captured;
    Remove(malformed, SessionClock::kTag); reject(malformed, meta);
    malformed = captured; Remove(malformed, SessionServices::kTag); reject(malformed, meta);
    malformed = captured; Remove(malformed, FileServices::kTag); reject(malformed, meta);
    malformed = captured; Find(malformed, SessionServices::kTag).flags = 0; reject(malformed, meta);
    malformed = captured; malformed.push_back(Find(malformed, FileServices::kTag)); reject(malformed, meta);
    auto wrong_meta = meta; ++wrong_meta.vsync_frame; reject(captured, wrong_meta);
    malformed = captured;
    SessionClock::Snapshot wrong_clock;
    CHECK(SessionClock::Decode(Find(malformed, SessionClock::kTag), wrong_clock, error));
    ++wrong_clock.domain.tick;
    CHECK(SessionClock::Encode(wrong_clock, Find(malformed, SessionClock::kTag).payload, error));
    reject(malformed, meta);
    // OLD SOUND_P5/LOADER_HLE: old work must not survive load, and saved work
    // must neither recharge its deadline nor publish before its bytes exist.
    const auto issued = StreamLibrary::Scheduling()->Count().issued;
    CHECK(restorer.RestoreImage(snapshot.data(), snapshot.size(), &error));
    CHECK(pre_restores == 1 && domain.DomainTick() == saved_tick && ticks.Pending() == 2);
    CHECK(StreamLibrary::FileGeneration(100) > saved_generation &&
          StreamLibrary::Scheduling()->Count().issued == issued);
    CHECK(StreamLibrary::Scheduling()->Capture(pending, &error) &&
          pending.reads.at(kFs).end_ms == deadline);
    CHECK(Sdk(title::Files::kSeek, {descriptor, 0, 1}) == 13);
    CHECK(memory::read<uint8_t>(kDest) == 0xce && memory::read<uint8_t>(kDest + 128) == 0xce);
    context.cpuRegs.GPR.r[4].UD[0] = kFs;
    CHECK(StreamLibrary::TryWait(context));
    CHECK(Bytes(kDest, 512, 128) && memory::read<uint8_t>(kDest + 128) == 0xce);
    CHECK(Sdk(title::Files::kRead, {descriptor, kDest + 256, 129}) == 129);
    CHECK(Bytes(kDest + 256, 13, 129));
    CHECK(original_calls == 0 && model.Enqueued() == 0);
    // Unselected optional service records previously disappeared during
    // StateRestorer filtering. Exercise the actual loader with both tags.
    StateRestorer inactive(memory_sections);
    CHECK(inactive.Registry().Register(clock.Section()));
    CHECK(off_join.RegisterValidation(inactive.Registry()));
    inactive.SetImageValidation([&](const auto& m, const auto& image, std::string* why) {
        return off_join.ValidateImage(m, image, true, why);
    });
    unsigned inactive_restores = 0;
    inactive.SetPreRestore([&](std::string*) { ++inactive_restores; return true; });
    for (uint32_t tag : {SessionServices::kTag, FileServices::kTag}) {
        malformed = captured;
        Remove(malformed, tag == SessionServices::kTag ? FileServices::kTag : SessionServices::kTag);
        Find(malformed, tag).flags = 0;
        const auto image = SerializeStateFile(meta, malformed);
        CHECK(!inactive.RestoreImage(image.data(), image.size(), &error) && inactive_restores == 0);
    }
    malformed = captured; Remove(malformed, FileServices::kTag); Remove(malformed, SessionServices::kTag);
    Remove(malformed, SessionClock::kTag);
    CHECK(off_join.ValidateImage(meta, malformed, true, &error));
    CHECK(!off_join.ValidateImage(meta, malformed, false, &error));
    Blob unknown{Tag('U', 'N', 'K', 'N'), 1, 0, {1}};
    malformed.push_back(unknown); malformed.push_back(unknown);
    const auto duplicate = SerializeStateFile(meta, malformed);
    CHECK(!inactive.RestoreImage(duplicate.data(), duplicate.size(), &error) && inactive_restores == 0);
    // --- the explicit legacy import (ServiceImport) ------------------------
    // A FROZEN legacy image: no GCLK/SVCS/FILS; the title's own StreamSystem in
    // RAM0 names one open ordinary file (handle 100, "data.bd") with a staged
    // ring fill pending; THRD shows the main thread in the game loop.
    constexpr uint32_t kSystem = 0x570000, kLinks = 0x571000, kStreams = 0x572000, kFsLegacy = 0x580000,
        kPathLegacy = 0x581000, kCbLegacy = 0x582000, kRingLegacy = 0x590000, kDestLegacy = 0x5a0000;
    std::vector<uint8_t> meta_arena(4096, 0x5a);
    Fill(kSystem, 0x1c, 0);
    memory::write<uint16_t>(kSystem, 4); memory::write<uint16_t>(kSystem + 4, 1);
    memory::write<uint32_t>(kSystem + 8, kLinks); memory::write<uint32_t>(kSystem + 12, kStreams);
    for (uint32_t slot = 0; slot < 4; ++slot) {
        memory::write<int16_t>(kLinks + slot * 2, slot == 2 ? -1 : -2);
        memory::write<uint32_t>(kStreams + slot * 4, slot == 2 ? kFsLegacy : 0);
    }
    memory::write<uint32_t>(ServiceImport::kStreamManagerPointer, kSystem);
    Fill(kFsLegacy, 0x48, 0);
    Text(kPathLegacy, "data.bd");
    memory::write<uint32_t>(kFsLegacy + 0x1c, kPathLegacy);
    memory::write<uint32_t>(kFsLegacy + 0x20, 7); memory::write<uint32_t>(kFsLegacy + 0x24, 8);
    memory::write<int32_t>(kFsLegacy + LoaderFileIo::kFsHandle, 100);
    memory::write<uint32_t>(kFsLegacy + LoaderFileIo::kFsFileSize, kSize);
    memory::write<uint32_t>(kFsLegacy + LoaderFileIo::kFsSeek, 0);
    memory::write<uint32_t>(kFsLegacy + LoaderFileIo::kFsRingStart, 1024);
    memory::write<uint32_t>(kFsLegacy + LoaderFileIo::kFsDest, kDestLegacy + 64);
    memory::write<uint32_t>(kFsLegacy + LoaderFileIo::kFsSize, 100);
    memory::write<uint32_t>(kFsLegacy + LoaderFileIo::kFsCbObj, kCbLegacy);
    memory::write<uint32_t>(kCbLegacy, kVtbl);
    memory::write<uint32_t>(kVtbl + 0x14, kCopy); memory::write<uint16_t>(kVtbl + 0x10, 0);
    memory::write<uint32_t>(kCbLegacy + LoaderFileIo::kCbBase, kRingLegacy);
    memory::write<uint32_t>(kCbLegacy + LoaderFileIo::kCbCursor, kRingLegacy);
    memory::write<uint32_t>(kCbLegacy + LoaderFileIo::kCbCap, 256);
    memory::write<uint32_t>(kFsLegacy, 2u | LoaderFileIo::kFlagReadPending | LoaderFileIo::kFlagRingValid);
    Fill(kRingLegacy, 256); Fill(kDestLegacy, 512);
    MemorySections legacy_memory(ram, scratch, meta_arena.data(), meta_arena.size());
    StateRestorer legacy_restorer(legacy_memory);
    legacy_restorer.RequireSectionOwners();
    const auto read_disc = [&](uint32_t lba, uint64_t position, uint32_t size, uint8_t* out) {
        std::vector<uint8_t> raw(((position % 2048 + size + 2047) / 2048) * 2048);
        if (reader.ReadSectors(lba + uint32_t(position / 2048), uint32_t(raw.size() / 2048), raw.data()) != raw.size() / 2048)
            return false;
        std::copy_n(raw.begin() + position % 2048, size, out);
        return true;
    };
    ServiceImport import(services, files, false, read_disc, [&] { return domain.DomainTick(); });
    ServiceRestore legacy_join(clock, services, files);
    legacy_join.SetImport(&import);
    unsigned published_files = 0, published_disc = 0;
    CHECK(legacy_restorer.Registry().Register(clock.Section()));
    CHECK(legacy_restorer.Registry().Register(services.Section([&] {
        std::string why;
        if (!import.PublishFiles(&why)) throw std::runtime_error(why);
        ++published_files;
    })));
    CHECK(legacy_restorer.Registry().Register(files.Section([&] {
        std::string why;
        if (!import.PublishDisc(&why)) throw std::runtime_error(why);
        ++published_disc;
    })));
    CHECK(legacy_join.RegisterValidation(legacy_restorer.Registry()));
    unsigned legacy_pre_restores = 0;
    legacy_restorer.SetPreRestore([&](std::string*) { ++legacy_pre_restores; return true; });
    legacy_restorer.SetImageValidation([&](const auto& m, const auto& image, std::string* why) {
        return legacy_join.ValidateImage(m, image, true, why);
    });
    legacy_restorer.AddImagePreflight([&](const auto& image, std::string* why) {
        std::string reason;
        return !import.Admits(image, &reason) || import.Prepare(image, why);
    }, [&] { import.Discard(); });
    // Every capture of the composed owners still emits GCLK, SVCS and FILS as REQUIRED.
    auto composed = legacy_restorer.Registry().Capture();
    CHECK(Find(composed, SessionClock::kTag).flags == kSectionRequired &&
          Find(composed, SessionServices::kTag).flags == kSectionRequired &&
          Find(composed, FileServices::kTag).flags == kSectionRequired);
    auto legacy = composed;
    Remove(legacy, SessionClock::kTag); Remove(legacy, SessionServices::kTag); Remove(legacy, FileServices::kTag);
    {
        ThreadSection::Record game;
        game.tid = 1; game.is_boundary = true; game.started = true;
        game.chain = {{0x1a0150, 0}, {0x17ce50, 0}};
        std::vector<uint8_t> threads;
        ThreadSection::Emit({game}, threads);
        legacy.push_back({TAG_THRD, 1, 0, std::move(threads)});   // optional here: no thread owner in this fixture
    }
    const uint64_t signature = ps2r::verify::MemoryCheckpointer::Hash(
        {{Find(legacy, TAG_RAM).payload.data(), Find(legacy, TAG_RAM).payload.size()},
         {Find(legacy, TAG_SPAD).payload.data(), Find(legacy, TAG_SPAD).payload.size()},
         {Find(legacy, TAG_META).payload.data(), Find(legacy, TAG_META).payload.size()}});
    FileMeta legacy_meta;
    legacy_meta.vsync_frame = 37156;
    const auto legacy_bytes = SerializeStateFile(legacy_meta, legacy);
    // Not inventoried: refused by name before any mutation.
    CHECK(!legacy_restorer.RestoreImage(legacy_bytes.data(), legacy_bytes.size(), &error) &&
          error.find("legacy service import: image is not an inventoried frozen anchor") != std::string::npos &&
          legacy_pre_restores == 0 && !StreamLibrary::Scheduling()->InFlight(kFsLegacy));
    import.Approve(signature);
    // A NEW image missing SVCS (it carries GCLK) is still a defect, not an import.
    malformed = composed; Remove(malformed, SessionServices::kTag);
    {
        const auto bytes = SerializeStateFile(legacy_meta, malformed);
        CHECK(!legacy_restorer.RestoreImage(bytes.data(), bytes.size(), &error) && legacy_pre_restores == 0);
    }
    // A saved chain inside the synchronous file family: a step in flight, refused by name.
    {
        auto stepping = legacy;
        ThreadSection::Record loader;
        loader.tid = 3; loader.started = true;
        loader.chain = {{0x2b4230, 0}};
        std::vector<uint8_t> threads;
        ThreadSection::Emit({loader}, threads);
        Find(stepping, TAG_THRD).payload = std::move(threads);
        const auto bytes = SerializeStateFile(legacy_meta, stepping);
        CHECK(!legacy_restorer.RestoreImage(bytes.data(), bytes.size(), &error) &&
              error.find("synchronous disc file step in flight") != std::string::npos && legacy_pre_restores == 0);
    }
    // The Audio-composed arm has no derivable state: refused by name.
    {
        ServiceImport audio_import(services, files, true, read_disc, [&] { return domain.DomainTick(); });
        audio_import.Approve(signature);
        CHECK(!audio_import.Prepare(legacy, &error) && error.find("owned Audio state") != std::string::npos);
    }
    // The admitted import: files derived from the title's StreamSystem, the
    // pending ring fill re-issued at the restore tick from the medium, zero
    // synchronous descriptors (the old one is invalid), no fabricated clock.
    const auto issued_before = StreamLibrary::Scheduling()->Count().issued;
    CHECK(legacy_restorer.RestoreImage(legacy_bytes.data(), legacy_bytes.size(), &error));
    CHECK(legacy_pre_restores == 1 && published_files == 1 && published_disc == 1 && !clock.Restored());
    CHECK(import.Count().admitted == 1 && import.Count().files == 1 && import.Count().pending_windows == 1);
    CHECK(StreamLibrary::FileOwned(100) && StreamLibrary::FileRegistered(100) &&
          StreamLibrary::Scheduling()->InFlight(kFsLegacy) &&
          StreamLibrary::Scheduling()->Count().issued == issued_before + 1);
    CHECK(memory::read<uint8_t>(kRingLegacy) == 0xce && memory::read<uint8_t>(kDestLegacy + 64) == 0xce);
    context.cpuRegs.GPR.r[4].UD[0] = kFsLegacy;
    LoaderFileIo::WaitReadComplete(context);   // the title's wait: delivery + its own completion copy
    CHECK(Bytes(kRingLegacy, 1024, 256));
    CHECK(Bytes(kDestLegacy + 64, 1024, 100));
    CHECK(!(memory::read<uint32_t>(kFsLegacy) & LoaderFileIo::kFlagReadPending));
    CHECK(Sdk(title::Files::kSeek, {descriptor, 0, 1}) < 0);
    CHECK(original_calls == 0 && model.Enqueued() == 0);
    // Capture after the import emits the complete new owner set.
    CHECK(legacy_join.CanCheckpoint(&error));
    composed = legacy_restorer.Registry().Capture();
    CHECK(Find(composed, SessionServices::kTag).flags == kSectionRequired &&
          Find(composed, FileServices::kTag).flags == kSectionRequired);
    LoaderFileIo::ResetForTest();
    unlink(path.c_str());
    std::printf("ServiceRestore: %d checks, %d failures; pending bytes and descriptors resumed, model calls=0; "
                "legacy import admitted=%llu refused=%llu\n",
                checks, failures, (unsigned long long)import.Count().admitted, (unsigned long long)import.Count().refused);
    return failures ? 1 : 0;
}
