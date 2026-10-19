#pragma once

#include "engine/core/savestate/section_registry.h"

class StateRestorer;
namespace ps2r::app {
class SessionClock;
class SessionServices;

// Joins the saved game clock to file/Audio deadlines before any owner changes.
// The registry path is also used by interactive reproduction preflight.
class ServiceClock {
public:
    ServiceClock(const SessionClock&, const SessionServices&);
    bool Register(StateRestorer&);
    bool Validate(const std::vector<savestate::Blob>&, std::string* error) const;

private:
    const SessionClock& clock_;
    const SessionServices& services_;
};
}  // namespace ps2r::app
