#pragma once

#include <cstdint>

#include "games/slus_20909_crash_twinsanity/hle/audio/audio.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

namespace title {

// Verified file/sample service ABI adapter. Recompiled GameSound loaders still
// choose IDs, ranges, allocation and readiness; this owner replaces their
// STREAM sample transfer and table commands. Music .mb streams are separate.
class SoundBanks {
public:
    using Capture = StreamLibrary::Capture;
    using Installer = StreamLibrary::Installer;
    // Non-null selects the owned arm, which requires StreamLibrary selected.
    // Startup/reset-only selection; an active asset owner cannot be detached.
    static bool Install(const Capture& capture, const Installer& install, Audio* selected);
    static bool Enabled();
    static bool Owns(const Audio& audio);
    // After StreamLibrary::Reset on composition teardown. Invalidates tickets.
    static void Reset();
    struct Counters {
        uint64_t issued = 0, delivered = 0, cancelled = 0, stale = 0,
                 reserved = 0, freed = 0, acknowledged = 0, errors = 0, fallbacks = 0;
    };
    static Counters Count();

private:
    static void Deliver(const FileSchedule::Payload& payload);
    static void Cancel(const FileSchedule::Payload& payload);
};

} // namespace title
