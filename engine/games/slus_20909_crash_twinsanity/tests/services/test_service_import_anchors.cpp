// ServiceImportAnchors: the explicit legacy import (ServiceImport) over every
// FROZEN anchor the title inventories (verify/restore_mhash_expected.tsv) --
// the OLD save_states and the coldboot b120 probe -- with the real disc
// mounted. No game process: the images are parsed, the import classifies,
// derives (StreamState::MigrateFiles over the image's own RAM0 at the title's
// StreamSystem pointer, the THRD chain guard), prepares and publishes into a
// fresh file owner. Every anchor must be admitted and publish; the derived
// handles are printed for the record. Skips (77) without the disc or anchors.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "games/slus_20909_crash_twinsanity/composition/file_services.h"
#include "games/slus_20909_crash_twinsanity/composition/service_import.h"
#include "games/slus_20909_crash_twinsanity/composition/session_services.h"
#include "engine/cdvd/delivery_budget.h"
#include "engine/cdvd/delivery_meter.h"
#include "engine/cdvd/hle/loader_model.h"
#include "engine/cdvd/iso_fs.h"
#include "engine/cdvd/read_engine.h"
#include "engine/core/config/config.h"
#include "engine/core/savestate/savestate_format.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/mem/address_map.h"
#include "engine/ee/mem/fast_mem.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/mmio_router.h"
#include "engine/ee/mem/mmio_sink.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/recomp_abi/recomp_abi.h"
#include "engine/ipu/hle/guest_memory_adapter.h"
#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

using namespace ps2r::app;
namespace {
int failures = 0, checks = 0;
#define CHECK(value, name) do { ++checks; if (!(value)) { ++failures; std::printf("FAIL %s: %s\n", name, error.c_str()); } \
    else std::printf("PASS %s\n", name); } while (false)
std::string error;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::printf("ServiceImportAnchors: SKIP (no disc/inventory)\n"); return 77; }
    const std::string disc = argv[1], inventory = argv[2], old_root = argv[3];
    const std::string probe_dir = argc > 4 ? argv[4] : "";
    if (!std::filesystem::exists(disc) || !std::filesystem::exists(inventory)) {
        std::printf("ServiceImportAnchors: SKIP (disc or inventory missing)\n"); return 77;
    }
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
    IsoFs iso;
    if (!iso.Open(disc)) { std::printf("ServiceImportAnchors: SKIP (disc does not open)\n"); return 77; }
    ReadEngine reader(iso);
    ps2r::CdvdConfig config;
    DeliveryMeter meter(config);
    DeliveryBudget budget(0);
    LoaderModel model(meter, budget);
    uint64_t tick = 1000;
    LoaderFileIo::ResetForTest();
    std::map<uint32_t, StreamLibrary::Body> dispatch;
    uint64_t original_calls = 0;
    if (!StreamLibrary::Install([&](uint32_t) -> StreamLibrary::Body {
            return [&](CpuContext&, uint32_t) { ++original_calls; };
        }, [&](uint32_t at, StreamLibrary::Body body) { dispatch[at] = std::move(body); return true; })) return 1;
    LoaderFileIo::Deps deps;
    deps.engine = &reader; deps.model = &model;
    deps.now_tick = [&] { return tick; };
    deps.park_frame = [&] { ++tick; };
    LoaderFileIo::Layout layout;
    LoaderFileIo::Bind(deps, layout);
    DispatchTable::Initialize();
    FileServices files(guest, iso, true);
    if (!files.Install()) return 1;
    SessionServices services(true, nullptr);
    const auto read_disc = [&](uint32_t lba, uint64_t position, uint32_t size, uint8_t* out) {
        std::vector<uint8_t> raw(((position % 2048 + size + 2047) / 2048) * 2048);
        if (reader.ReadSectors(lba + uint32_t(position / 2048), uint32_t(raw.size() / 2048), raw.data()) != raw.size() / 2048)
            return false;
        std::copy_n(raw.begin() + position % 2048, size, out);
        return true;
    };
    unsigned anchors = 0;
    std::ifstream rows(inventory);
    std::string line;
    while (std::getline(rows, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string name;
        row >> name;
        std::string path = old_root + "/save_states/" + name;
        if (!std::filesystem::exists(path) && !probe_dir.empty()) path = probe_dir + "/" + name;
        if (!std::filesystem::exists(path)) { std::printf("SKIP %s: anchor file absent\n", name.c_str()); continue; }
        ++anchors;
        ps2r::savestate::FileMeta meta;
        std::vector<ps2r::savestate::Blob> image;
        if (!ps2r::savestate::ReadStateFile(path, &meta, &image, &error)) { CHECK(false, name.c_str()); continue; }
        ServiceImport import(services, files, false, read_disc, [&] { return tick; });
        import.Approve(inventory, path);
        std::string why;
        CHECK(ServiceImport::LegacySchema(image, &why), (name + ": legacy schema").c_str());
        error = why;
        CHECK(import.Admits(image, &why), (name + ": admitted (inventoried signature)").c_str());
        error = why;
        LoaderFileIo::ResetState();
        const bool prepared = import.Prepare(image, &error);
        CHECK(prepared, (name + ": prepared (files derived from the title's StreamSystem)").c_str());
        if (!prepared) continue;
        CHECK(import.PublishFiles(&error), (name + ": files published").c_str());
        CHECK(import.PublishDisc(&error), (name + ": disc files published (zero descriptors)").c_str());
        LoaderFileIo::State live;
        CHECK(LoaderFileIo::CaptureState(live) && live.files.size() == import.Count().files,
              (name + ": published owner holds the derived handles").c_str());
        std::string handles;
        for (const auto& file : live.files)
            handles += " " + std::to_string(file.first) + "=" + file.second.path + "@" + std::to_string(file.second.lba) +
                       "+" + std::to_string(file.second.size) + (StreamLibrary::FileRegistered(file.first) ? "(registered)" : "");
        std::printf("  %s vsync_frame=%u files=%llu pending_windows=%llu handles:%s\n", name.c_str(), meta.vsync_frame,
                    (unsigned long long)import.Count().files, (unsigned long long)import.Count().pending_windows, handles.c_str());
        // A tampered signature refuses by name, before any mutation.
        ServiceImport other(services, files, false, read_disc, [&] { return tick; });
        other.Approve(1);
        CHECK(!other.Admits(image, &why) && why.find("inventoried frozen signature") != std::string::npos,
              (name + ": a foreign signature refuses by name").c_str());
    }
    CHECK(anchors > 0, "inventory names at least one present anchor");
    CHECK(original_calls == 0 && model.Enqueued() == 0, "no original call and no model enqueue during import");
    LoaderFileIo::ResetForTest();
    std::printf("ServiceImportAnchors: %u anchors, %d checks, %d failures\n", anchors, checks, failures);
    return failures ? 1 : 0;
}
