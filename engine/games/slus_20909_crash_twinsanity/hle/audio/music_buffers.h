#pragma once

#include "games/slus_20909_crash_twinsanity/hle/stream/stream_buffers.h"
#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

namespace title {

// Buffer service ABI. Install after MusicLibrary, only when both Movie
// consumers resolve/return these host loans. The gameplay's cached buffer
// value becomes an opaque handle; it must never reach retained IOP consumers.
class MusicBuffers {
public:
    using Capture = StreamLibrary::Capture;
    using Installer = StreamLibrary::Installer;
    static bool Install(const Capture& capture, const Installer& install, StreamBuffers* selected);
    static bool Enabled();
    // Discard the SoundControls batch first, stop MusicLibrary, return both
    // Movie loans, then reset buffers. Refuses outstanding Movie views.
    static bool Reset();
    static bool CanCheckpoint();
    static bool CanPlay(uint32_t stream);
    // The stream's allocated IOP buffer bytes (the title's AttachBuffer size:
    // two blocks of every track), 0 when no buffers owner is composed or the
    // slot is not allocated. The music service fills this much before readiness.
    static uint32_t BufferBytes(uint32_t stream);
    static StreamBuffers::Loan BorrowMovie(CpuContext& ctx, uint32_t handle, StreamBuffers::MoviePart part);
    static StreamBuffers::View Resolve(const StreamBuffers::Loan& loan);
    static bool ReturnMovie(const StreamBuffers::Loan& loan);
    struct State {
        StreamBuffers::Snapshot storage;
        std::array<bool, StreamBuffers::kStreams> selected{};
        uint32_t free_word = 0, free_value = 0;
    };
    // Component snapshots; SoundControls' pending BufferRequests and Movie's
    // two saved tokens must be composed separately. These calls never Flush.
    static bool CaptureState(State& state, std::string* error = nullptr);
    static bool ValidateState(const State& state, std::string* error = nullptr);
    static bool RestoreState(const State& state, StreamBuffers::RenewedLoans& loans,
                             std::string* error = nullptr);
    struct Counters {
        uint64_t allocations = 0, resizes = 0, closes = 0, queries = 0,
                 sound_loans = 0, disc_loans = 0, returns = 0,
                 errors = 0, fallback = 0, statuses = 0;
    };
    static Counters Count();
};
} // namespace title
