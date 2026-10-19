#include "games/slus_20909_crash_twinsanity/composition/file_services.h"

#include <stdexcept>
#include <utility>

#include "engine/core/verify/run_meta.h"
#include "engine/ee/cpu/dispatch_table.h"

namespace ps2r::app {
namespace {
bool Fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
}

FileServices::FileServices(ipu::GuestMemory& memory, const IsoFs& disc, bool selected)
    : files_(memory, disc), selected_(selected) {}

bool FileServices::Install() {
    return files_.Install(DispatchTable::Capture, DispatchTable::Install, selected_);
}

bool FileServices::Decode(const savestate::Blob& blob, ::title::Files::State& state,
                          std::string* error) const {
    if (!selected_ || !files_.Selected() || blob.tag != kTag || blob.version != 1 ||
        blob.flags != savestate::kSectionRequired)
        return Fail(error, "disc file section envelope or selection differs");
    return files_.Decode(blob.payload, state, error);
}

bool FileServices::ValidateImage(const std::vector<savestate::Blob>& image, std::string* error) const {
    const savestate::Blob* found = nullptr;
    for (const auto& blob : image) if (blob.tag == kTag) {
        if (found) return Fail(error, "duplicate disc file section");
        found = &blob;
    }
    if (!selected_) return !found || Fail(error, "saved disc file owner is not selected");
    if (!found) return Fail(error, "selected disc files require FILS");
    ::title::Files::State state;
    return Decode(*found, state, error);
}

bool FileServices::CanCheckpoint(std::string* error) const {
    if (!selected_) return true;
    if (!files_.Selected()) return Fail(error, "selected disc file owner is not installed");
    std::vector<uint8_t> bytes;
    return files_.Encode(bytes, error);
}

bool FileServices::ImportLegacy(std::string* error) {
    if (!selected_ || !files_.Selected()) return Fail(error, "selected disc file owner is not installed");
    ::title::Files::State state = files_.Snapshot();
    state.files.clear();
    return files_.Restore(state, error);
}

savestate::SectionSpec FileServices::Section(std::function<void()> legacy_absent) {
    savestate::SectionSpec spec;
    spec.tag = kTag; spec.version = 1; spec.owner = "FileServices";
    spec.required = !legacy_absent; spec.capture_required = true;
    spec.absent = std::move(legacy_absent);
    spec.serialize = [this](std::vector<uint8_t>& bytes) {
        std::string error;
        if (!selected_ || !files_.Selected() || !files_.Encode(bytes, &error))
            throw std::runtime_error("disc file capture: " + error);
    };
    spec.validate = [this](const savestate::Blob& blob, std::string* error) {
        ::title::Files::State state;
        return Decode(blob, state, error);
    };
    spec.restore = [this](const savestate::Blob& blob, std::string* error) {
        ::title::Files::State state;
        return Decode(blob, state, error) && files_.Restore(state, error);
    };
    return spec;
}

void FileServices::Record(verify::RunMeta& meta) const {
    meta.Set("files", files_.Census());
}

} // namespace ps2r::app
