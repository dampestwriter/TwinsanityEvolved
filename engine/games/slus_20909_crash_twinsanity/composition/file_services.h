#pragma once

#include <functional>

#include "engine/core/savestate/section_registry.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"

namespace ps2r::verify { class RunMeta; }

namespace ps2r::app {

// Application lifetime and REQUIRED state for synchronous disc files.
// There are no pending jobs or callbacks to join with the logical clock.
class FileServices {
public:
    static constexpr uint32_t kTag = 0x534c4946; // FILS, distinct from legacy FILE
    FileServices(ipu::GuestMemory& memory, const IsoFs& disc, bool selected);
    bool Install();
    bool Enabled() const { return selected_; }
    // With `legacy_absent`, FILS stays required of every capture but a frozen
    // legacy image may omit it: the callback publishes the explicit import
    // (ServiceImport) at this owner's restore position.
    savestate::SectionSpec Section(std::function<void()> legacy_absent = {});
    bool ValidateImage(const std::vector<savestate::Blob>& image, std::string* error) const;
    // The legacy import's publication: the mounted medium's identity with no
    // open descriptor (the title keeps none across a frame).
    bool ImportLegacy(std::string* error);
    // Pure observational capture check. Root must also install ValidateImage
    // through ServiceRestore before admitting incoming service snapshots.
    bool CanCheckpoint(std::string* error) const;
    void Record(verify::RunMeta& meta) const;

private:
    bool Decode(const savestate::Blob& blob, ::title::Files::State& state, std::string* error) const;
    ::title::Files files_;
    bool selected_ = false;
};

} // namespace ps2r::app
