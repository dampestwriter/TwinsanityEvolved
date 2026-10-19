#pragma once

#include "engine/core/savestate/section_registry.h"

namespace ps2r::app {
class FileServices;
class ServiceImport;
class SessionClock;
class SessionServices;

// Application join for the clock and selected service owners. Checks the
// original image before a partial-owner loader can discard unknown records.
// Providers outlive this adapter; validation performs no reads or callbacks.
class ServiceRestore {
public:
    ServiceRestore(SessionClock& clock, SessionServices& services, FileServices& files);
    // The explicit legacy import: a frozen image it admits (ServiceImport::
    // Admits) restores under the selected composition with its owners' state
    // derived from the image; the import prepares in the preflight and
    // publishes at the owners' absent positions. Without it, legacy selected
    // restores stay refused.
    void SetImport(ServiceImport* import) { import_ = import; }
    bool RegisterValidation(savestate::SectionRegistry& registry);
    bool ValidateOwners(const std::vector<savestate::Blob>& image, std::string* error) const;
    bool ValidateImage(const savestate::FileMeta& meta, const std::vector<savestate::Blob>& image,
                       bool clockless_import, std::string* error) const;
    bool CanCheckpoint(std::string* error) const;

private:
    bool Legacy(const std::vector<savestate::Blob>& image) const;
    SessionClock& clock_;
    SessionServices& services_;
    FileServices& files_;
    ServiceImport* import_ = nullptr;
};
} // namespace ps2r::app
