#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/saves/save_storage.h"

namespace title {

// Complete NTSC save-manager entries and the startup service interface.
// Existing game serialization,
// save-device state machines and UI callbacks remain their original owners.
class SaveBindings {
public:
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    struct Hooks {
        std::function<bool(uint32_t, void*, size_t)> read;
        std::function<bool(uint32_t, const void*, size_t)> write;
        std::function<uint64_t()> boundary;
    };
    struct Outputs {
        uint32_t value = 0, entry = 0, data = 0, size = 0;
    };
    struct Snapshot {
        uint32_t version = 1;
        SaveStorage::Snapshot storage;
        Outputs outputs;
    };
    struct Counters {
        uint64_t calls = 0, submitted = 0, published = 0, rejected = 0;
        uint64_t starts = 0, start_failures = 0;
    };
    SaveBindings(SaveStorage& storage, Hooks hooks);
    // Install after SDK bindings. These bodies never invoke the replaced
    // manager/SDK continuation. On partial failure the root must stop startup.
    bool Install(const Installer& installer);
    void Invoke(uint32_t function, CpuContext& context);
    std::optional<Snapshot> Capture() const;
    static bool ValidateSnapshot(const Snapshot& snapshot, std::string* error = nullptr);
    bool Restore(const Snapshot& snapshot, uint64_t boundary);
    void Reset();
    Counters Counts() const { return counts_; }
    // Game memory an entry may read or write: main RAM (any segment) or the
    // scratchpad, where the title's main thread keeps its stack.
    static bool RamRange(uint32_t address, size_t size);

private:
    bool String(uint32_t address, size_t max, std::string& out) const;
    void Write(uint32_t address, const void* bytes, size_t size) const;
    void Publish();
    SaveStorage& storage_;
    Hooks hooks_;
    Outputs outputs_;
    Counters counts_;
};

}  // namespace title
