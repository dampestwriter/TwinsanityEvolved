#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"

class IsoFs;
namespace ps2r::ipu { class GuestMemory; }

namespace title {

// Synchronous disc-file intents used by Platform::Files. Descriptors and
// cursors belong to this owner; IsoFs supplies bytes, never an IOP fd table.
// Calls and snapshots run on the game owner with no callbacks or clock pump.
class Files {
public:
    static constexpr uint32_t kReset = 0x002d3948, kOpen = 0x002d3980,
        kClose = 0x002d3c10, kSeek = 0x002d3d88, kRead = 0x002d3fc8, kWrite = 0x002d4238;
    // Every function that holds a synchronous descriptor between its open and
    // its close: the six SDK bodies above, the title's File adapters that call
    // them (TR src/game/stream.cpp:273/282/290/95/110-143 -> NTSC 0x2b4230
    // open, 0x2b62a0 close, 0x2b62e0 read, 0x2b6378 write, 0x2b63b0 seek) and
    // the disc-error probe that opens and closes the check file in one step
    // (TR src/platform/ps2/stream.cpp:99-105 -> NTSC 0x2c5ff0). A saved call
    // chain inside one of these is a synchronous file step in flight.
    static constexpr std::array<uint32_t, 12> kStepFunctions{
        kReset, kOpen, kClose, kSeek, kRead, kWrite,
        0x002b4230, 0x002b62a0, 0x002b62e0, 0x002b6378, 0x002b63b0, 0x002c5ff0};
    static constexpr size_t kMaxFiles = 32, kMaxPath = 1024;
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    struct File {
        uint32_t handle = 0, location = 0, size = 0, position = 0;
        std::string path;
    };
    struct State {
        uint64_t image_bytes = 0;
        uint32_t volume_sectors = 0, root_location = 0, root_bytes = 0;
        std::string volume_id;
        std::vector<File> files;
    };
    struct Counters {
        uint64_t opens = 0, closes = 0, reads = 0, seeks = 0, resets = 0,
            bytes = 0, errors = 0, writes_refused = 0;
        std::array<uint64_t, 6> original_calls{};
    };

    Files(ps2r::ipu::GuestMemory& memory, const IsoFs& disc);
    bool Install(const Capture& capture, const Installer& install, bool selected);
    bool Selected() const { return selected_; }
    int32_t Open(const std::string& path, int32_t flags);
    int32_t Close(int32_t handle);
    int32_t Read(int32_t handle, uint32_t destination, int32_t bytes);
    int32_t Seek(int32_t handle, int32_t offset, uint32_t whence);
    int32_t Write(int32_t handle);
    void Reset();
    uint64_t Generation(int32_t handle) const;
    State Snapshot() const;
    bool Validate(const State& state, std::string* error = nullptr) const;
    bool Restore(const State& state, std::string* error = nullptr);
    bool Encode(std::vector<uint8_t>& bytes, std::string* error = nullptr) const;
    bool Decode(const std::vector<uint8_t>& bytes, State& state, std::string* error = nullptr) const;
    Counters Count() const { return counts_; }
    std::string Census() const;

private:
    struct Slot { File file; uint64_t generation = 0; };
    Slot* Find(int32_t handle);
    int32_t Error(int32_t result);
    bool Path(uint32_t address, std::string& path) const;
    static bool DiscPath(const std::string& path);
    void Call(size_t index, CpuContext& context);
    ps2r::ipu::GuestMemory& memory_;
    const IsoFs& disc_;
    std::array<Slot, kMaxFiles> slots_{};
    uint64_t next_generation_ = 1;
    bool installed_ = false, selected_ = false;
    Counters counts_;
};

} // namespace title
