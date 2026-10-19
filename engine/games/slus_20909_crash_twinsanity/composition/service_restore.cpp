#include "games/slus_20909_crash_twinsanity/composition/service_restore.h"

#include "games/slus_20909_crash_twinsanity/composition/file_services.h"
#include "games/slus_20909_crash_twinsanity/composition/service_import.h"
#include "app/host/session_clock.h"
#include "games/slus_20909_crash_twinsanity/composition/session_services.h"

namespace ps2r::app {
namespace {
bool Fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
}

ServiceRestore::ServiceRestore(SessionClock& clock, SessionServices& services, FileServices& files)
    : clock_(clock), services_(services), files_(files) {}

bool ServiceRestore::RegisterValidation(savestate::SectionRegistry& registry) {
    return registry.RegisterValidation("ServiceRestore", [this](const auto& image, std::string* error) {
        return ValidateOwners(image, error);
    });
}

bool ServiceRestore::Legacy(const std::vector<savestate::Blob>& image) const {
    if (!import_ || (!services_.Enabled() && !files_.Enabled())) return false;
    std::string why;
    return import_->Admits(image, &why);
}

bool ServiceRestore::ValidateOwners(const std::vector<savestate::Blob>& image, std::string* error) const {
    // A frozen legacy image the import admits: its owners' sections are
    // absent by schema; the import's preflight derives them (Prepare). A
    // legacy-schema image the import refuses is named as such.
    if (import_ && (services_.Enabled() || files_.Enabled())) {
        std::string why;
        if (import_->Admits(image, &why)) return true;
        if (ServiceImport::LegacySchema(image, nullptr)) return Fail(error, why.c_str());
    }
    if (!files_.ValidateImage(image, error)) return false;
    if (!services_.Enabled()) return services_.ValidateImage(image, 0, error);
    const savestate::Blob* found = nullptr;
    for (const auto& blob : image) if (blob.tag == SessionClock::kTag) {
        if (found) return Fail(error, "duplicate service deadline clock");
        found = &blob;
    }
    if (!found) return Fail(error, "selected service deadlines require GCLK");
    SessionClock::Snapshot clock;
    std::string reason;
    if (!SessionClock::Decode(*found, clock, reason) || !clock_.Validate(clock, reason)) {
        if (error) *error = reason;
        return false;
    }
    return services_.ValidateImage(image, clock.domain.tick, error);
}

bool ServiceRestore::ValidateImage(const savestate::FileMeta& meta,
                                   const std::vector<savestate::Blob>& image,
                                   bool clockless_import, std::string* error) const {
    if (!ValidateOwners(image, error)) return false;
    const bool legacy = Legacy(image);
    if (legacy && !clockless_import) return Fail(error, "legacy service import is a boot operation");
    std::string reason;
    // The import's clock is M0's historical boot policy (clockless), the same
    // clock the retained arm restores these anchors under.
    if (!clock_.ValidateBootImage(meta, image, clockless_import && (!services_.Enabled() || legacy), reason)) {
        if (error) *error = reason;
        return false;
    }
    return true;
}

bool ServiceRestore::CanCheckpoint(std::string* error) const {
    return services_.CanCheckpoint(error) && files_.CanCheckpoint(error);
}
} // namespace ps2r::app
