#include "games/slus_20909_crash_twinsanity/composition/service_import.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include "games/slus_20909_crash_twinsanity/composition/file_services.h"
#include "app/host/session_clock.h"
#include "games/slus_20909_crash_twinsanity/composition/session_services.h"
#include "engine/core/verify/memory_checkpointer.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/savestate/thread_section.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"

namespace ps2r::app {
namespace {
using savestate::Blob;

// The frozen OLD schema (SNDC optional). Any other tag is a new owner's
// record: an image carrying one is a NEW image missing a section, never a
// legacy import candidate.
constexpr std::array<uint32_t, 30> kFrozenTags{
    savestate::TAG_RAM,  savestate::TAG_SPAD, savestate::TAG_META, savestate::TAG_VRAM, savestate::TAG_VU0C,
    savestate::TAG_VU0D, savestate::TAG_VU1C, savestate::TAG_VU1D, savestate::TAG_THRD, savestate::TAG_SEMA,
    savestate::TAG_EVFL, savestate::TAG_INTC, savestate::TAG_DMAC, savestate::TAG_TIMR, savestate::TAG_GSPR,
    savestate::TAG_GSST, savestate::TAG_VIFS, savestate::TAG_GIFS, savestate::TAG_PADS, savestate::TAG_FILE,
    savestate::TAG_CNTR, savestate::TAG_MISC, savestate::TAG_HEAP, savestate::TAG_STRM, savestate::TAG_SIFS,
    savestate::TAG_VU1R, savestate::TAG_SDRX, savestate::TAG_MCHL, savestate::TAG_IOPM, savestate::TAG_SNDC};

const Blob* Find(const std::vector<Blob>& image, uint32_t tag) {
    for (const auto& blob : image) if (blob.tag == tag) return &blob;
    return nullptr;
}
} // namespace

ServiceImport::ServiceImport(SessionServices& services, FileServices& files, bool audio_selected,
                             StreamState::ReadDisc read_disc, std::function<uint64_t()> tick)
    : services_(services), files_(files), audio_selected_(audio_selected),
      read_disc_(std::move(read_disc)), tick_(std::move(tick)) {}

void ServiceImport::Approve(const std::string& inventory, const std::string& load_file) {
    approved_ = 0;
    if (load_file.empty()) return;
    const auto name = std::filesystem::path(load_file).filename();
    std::ifstream input(inventory);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string file;
        uint64_t value = 0;
        if (row >> file >> std::hex >> value && std::filesystem::path(file).filename() == name) {
            approved_ = value;
            return;
        }
    }
}

bool ServiceImport::Refuse(std::string* error, const std::string& why) {
    refusal_ = "legacy service import: " + why;
    ++census_.refused;
    if (error) *error = refusal_;
    return false;
}

bool ServiceImport::LegacySchema(const std::vector<Blob>& image, std::string* why) {
    const auto no = [&](const char* reason) { if (why) *why = reason; return false; };
    if (Find(image, SessionClock::kTag) || Find(image, SessionServices::kTag) || Find(image, FileServices::kTag))
        return no("image carries new service state");
    for (const auto& blob : image)
        if (std::find(kFrozenTags.begin(), kFrozenTags.end(), blob.tag) == kFrozenTags.end())
            return no("image carries a new owner's section");
    return true;
}

bool ServiceImport::Admits(const std::vector<Blob>& image, std::string* why) const {
    if (!LegacySchema(image, why)) return false;
    const auto no = [&](const char* reason) {
        refusal_ = std::string("legacy service import: ") + reason;
        if (why) *why = refusal_;
        return false;
    };
    const Blob* ram = Find(image, savestate::TAG_RAM);
    const Blob* spad = Find(image, savestate::TAG_SPAD);
    const Blob* meta = Find(image, savestate::TAG_META);
    if (!ram || !spad || !meta || ram->payload.empty() || spad->payload.empty() || meta->payload.empty())
        return no("image lacks the frozen memory sections");
    if (!approved_) return no("image is not an inventoried frozen anchor");
    const std::vector<verify::MemorySpan> spans{{ram->payload.data(), ram->payload.size()},
                                                {spad->payload.data(), spad->payload.size()},
                                                {meta->payload.data(), meta->payload.size()}};
    if (verify::MemoryCheckpointer::Hash(spans) != approved_)
        return no("image memory differs from its inventoried frozen signature");
    return true;
}

bool ServiceImport::Derive(const std::vector<Blob>& image, LoaderFileIo::State& state,
                           std::vector<StreamState::PendingWindow>& pending, std::string* error) const {
    const Blob* ram = Find(image, savestate::TAG_RAM);
    if (!ram) { if (error) *error = "image lacks RAM0"; return false; }
    const std::vector<uint8_t>& bytes = ram->payload;
    const StreamState::ReadMemory read = [&bytes](uint32_t address, uint8_t* out, size_t size) {
        if (address > bytes.size() || size > bytes.size() - address) return false;
        std::copy_n(bytes.begin() + address, size, out);
        return true;
    };
    uint8_t word[4]{};
    if (!read(kStreamManagerPointer, word, sizeof(word))) { if (error) *error = "image RAM0 is shorter than the title's data"; return false; }
    const uint32_t system = static_cast<uint32_t>(word[0]) | static_cast<uint32_t>(word[1]) << 8 |
                            static_cast<uint32_t>(word[2]) << 16 | static_cast<uint32_t>(word[3]) << 24;
    if (!system) {
        // Before FUN_002ad560 the title has no stream manager: nothing is open.
        state = LoaderFileIo::State{};
        state.streams.enabled = true;
        pending.clear();
        return LoaderFileIo::ValidateState(state, error);
    }
    return StreamState::MigrateFiles(system, read, state, pending, error);
}

bool ServiceImport::Prepare(const std::vector<Blob>& image, std::string* error) {
    Discard();
    std::string why;
    if (!Admits(image, &why)) return Refuse(error, why);
    if (audio_selected_)
        return Refuse(error, "the image carries no owned Audio state (sound_samples/music select the composed arm)");
    LoaderFileIo::State state;
    std::vector<StreamState::PendingWindow> pending;
    if (!Derive(image, state, pending, &why)) return Refuse(error, "files: " + why);
    if (files_.Enabled()) {
        // The synchronous family: a saved chain inside it is a step in flight.
        if (const Blob* threads = Find(image, savestate::TAG_THRD)) {
            std::vector<ThreadSection::Record> records;
            if (!ThreadSection::Parse(threads->payload, records, &why)) return Refuse(error, "threads: " + why);
            const auto& steps = ::title::Files::kStepFunctions;
            for (const auto& record : records) {
                bool inside = record.has_ctx &&
                    std::find(steps.begin(), steps.end(), record.ctx.cpuRegs.pc) != steps.end();
                for (const auto& frame : record.chain)
                    inside = inside || std::find(steps.begin(), steps.end(), frame.func_addr) != steps.end();
                if (inside)
                    return Refuse(error, "disc files: thread " + std::to_string(record.tid) +
                                         " has a synchronous disc file step in flight");
            }
        }
    }
    std::map<Key, std::vector<uint8_t>> staged;
    for (const auto& window : pending) {
        const auto file = state.files.find(window.handle);
        if (file == state.files.end()) return Refuse(error, "files: pending window has no file identity");
        std::vector<uint8_t> bytes(window.fill);
        if (!read_disc_ || !read_disc_(file->second.lba, window.position, window.fill, bytes.data()))
            return Refuse(error, "files: pending window bytes are not readable from the mounted medium");
        staged[Key{file->second.lba, window.position, window.fill}] = std::move(bytes);
    }
    state_ = std::move(state);
    pending_ = std::move(pending);
    staged_ = std::move(staged);
    prepared_ = true;
    ++census_.admitted;
    census_.files = state_.files.size();
    census_.pending_windows = pending_.size();
    return true;
}

void ServiceImport::Discard() {
    prepared_ = false;
    state_ = LoaderFileIo::State{};
    pending_.clear();
    staged_.clear();
}

bool ServiceImport::PublishFiles(std::string* error) {
    if (!prepared_) return Refuse(error, "no prepared import to publish");
    if (!services_.ImportFiles(state_, error)) return false;
    // The bytes were read at preparation; the same (lba, position, fill)
    // names the same bytes, so a lookup replaces the medium here and the
    // all-or-nothing import cannot fail on I/O.
    const auto& staged = staged_;
    const StreamState::ReadDisc from_staged = [&staged](uint32_t lba, uint64_t position, uint32_t fill, uint8_t* out) {
        const auto found = staged.find(Key{lba, position, fill});
        if (found == staged.end()) return false;
        std::copy(found->second.begin(), found->second.end(), out);
        return true;
    };
    if (!pending_.empty() && !StreamState::ImportPending(pending_, state_, from_staged, tick_ ? tick_() : 0, error))
        return false;
    ++census_.files_published;
    return true;
}

bool ServiceImport::PublishDisc(std::string* error) {
    if (!prepared_) return Refuse(error, "no prepared import to publish");
    if (!files_.ImportLegacy(error)) return false;
    ++census_.disc_published;
    return true;
}

void ServiceImport::Record(verify::RunMeta& meta) const {
    meta.Set("service_import", "admitted=" + std::to_string(census_.admitted) +
             ",refused=" + std::to_string(census_.refused) +
             ",files_published=" + std::to_string(census_.files_published) +
             ",disc_published=" + std::to_string(census_.disc_published) +
             ",files=" + std::to_string(census_.files) +
             ",pending_windows=" + std::to_string(census_.pending_windows) +
             ",last_refusal=" + (refusal_.empty() ? std::string("none") : refusal_));
}

} // namespace ps2r::app
