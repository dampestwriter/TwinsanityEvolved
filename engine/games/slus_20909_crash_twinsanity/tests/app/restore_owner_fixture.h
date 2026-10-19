#pragma once

#include <atomic>
#include <initializer_list>
#include <map>

#include "games/slus_20909_crash_twinsanity/composition/input_save.h"
#include "engine/ee/mem/scratchpad.h"
#include "app/host/session_clock.h"

// Real selected owners for the SDL/F5/F8 transaction test. Only the adopted
// game owner invokes their bindings. The director queues host requests and
// observes atomics; it never reads mutable service state across threads.
class RestoreOwnerFixture {
public:
    RestoreOwnerFixture(MainMemory&, EmuDomain&, TickSource&, const std::string& directory);
    bool Register(ps2r::savestate::SectionRegistry&, ps2r::app::SessionClock&);
    bool Start();
    void Step();
    bool CanCheckpoint(std::string& error) const;
    bool Restored() const;
    void QueueSetup();
    void QueueWrite();
    bool WriteDone() const { return write_done_.load(); }
    unsigned Commits() const { return commits_.load(); }
    const std::string& File() const { return file_; }

private:
    int64_t Call(uint32_t address, std::initializer_list<uint32_t> args);
    bool Finish();
    MainMemory& ram_;
    Scratchpad spad_;
    EmuDomain& domain_;
    TickSource& source_;
    std::atomic<bool> setup_{false}, write_{false}, write_done_{false};
    std::atomic<unsigned> commits_{0};
    ps2r::app::InputSave owners_;
    std::map<uint32_t, ::title::InputBindings::Body> entries_;
    const std::string file_;
    bool write_pending_ = false; // host test command, completed before any F8
};
