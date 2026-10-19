#include "games/slus_20909_crash_twinsanity/composition/service_clock.h"

#include "app/host/session_clock.h"
#include "games/slus_20909_crash_twinsanity/composition/session_services.h"
#include "engine/ee/savestate/state_restorer.h"

namespace ps2r::app {
ServiceClock::ServiceClock(const SessionClock& clock, const SessionServices& services)
    : clock_(clock), services_(services) {}

bool ServiceClock::Validate(const std::vector<savestate::Blob>& blobs, std::string* error) const {
    if (!services_.Enabled()) return services_.ValidateImage(blobs, 0, error);
    const savestate::Blob* saved_clock = nullptr;
    for (const auto& blob : blobs) if (blob.tag == SessionClock::kTag) {
        if (saved_clock) {
            if (error) *error = "Service deadlines have duplicate saved game clocks";
            return false;
        }
        saved_clock = &blob;
    }
    if (!saved_clock) {
        if (error) *error = "Selected services require their saved game clock";
        return false;
    }
    SessionClock::Snapshot state;
    std::string reason;
    if (!SessionClock::Decode(*saved_clock, state, reason) || !clock_.Validate(state, reason)) {
        if (error) *error = reason;
        return false;
    }
    return services_.ValidateImage(blobs, state.domain.tick, error);
}

bool ServiceClock::Register(StateRestorer& restorer) {
    const auto check = [this](const auto& blobs, std::string* error) { return Validate(blobs, error); };
    if (!restorer.Registry().RegisterValidation("ServiceClock", check)) return false;
    // StateRestorer filters unowned optional sections before registry checks.
    // Also inspect the unfiltered image so deselection cannot hide SVCS.
    restorer.AddImagePreflight(check);
    return true;
}
}  // namespace ps2r::app
