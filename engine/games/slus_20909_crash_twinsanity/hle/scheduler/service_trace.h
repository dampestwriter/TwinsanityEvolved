#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

namespace title {

// Read-only attribution at verified service entries. Install after the chosen
// owners in either arm. Every original body runs exactly once, with the same
// context/entry. No gameplay binding, alternate owner, clock or pump is added.
class ServiceTrace {
public:
    struct Position {
        uint64_t presented = 0, domain = 0, service_us = 0, service_frame_us = 0, replies = 0;
        double read_busy_ms = 0, read_end_ms = 0;
        uint32_t read_head = 0;
    };
    struct Event {
        uint64_t call = 0, parent = 0;
        uint32_t depth = 0, address = 0, entry = 0, caller = 0;
        std::array<uint32_t, 8> arguments{};
        int64_t result = 0;
        bool leaving = false;
        Position position;
    };
    using Snapshot = std::function<Position()>;
    using Sink = std::function<void(const Event&)>;

    ServiceTrace(Snapshot snapshot, Sink sink);
    ~ServiceTrace();
    ServiceTrace(const ServiceTrace&) = delete;
    ServiceTrace& operator=(const ServiceTrace&) = delete;
    bool Install(const StreamLibrary::Capture& capture, const StreamLibrary::Installer& install);
    // Clears callbacks before root clock/log dependencies die. Captured wrappers
    // remain safe passthroughs until the dispatch table is replaced.
    void Disable();
    uint64_t ObserverFailures() const;
    static std::string Format(const Event& event);

private:
    struct State;
    static void Observe(State& state, Event& event);
    static void Invoke(const std::shared_ptr<State>& state, const StreamLibrary::Body& original,
                       uint32_t address, CpuContext& context, uint32_t entry);
    std::shared_ptr<State> state_;
};
} // namespace title
