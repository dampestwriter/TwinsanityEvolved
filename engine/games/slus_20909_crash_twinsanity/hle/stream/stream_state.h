#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/loader_sync/loader_file_io.h"

// Portable file-service payload, independent of a savestate container or any
// PS2 device section. RAM/clock restoration belongs to the composition owner.
class StreamState {
public:
    // Read-only game address space, e.g. an incoming RAM0 section during
    // preflight. The provider checks its own backing extent and copies bytes.
    using ReadMemory = std::function<bool(uint32_t, uint8_t*, size_t)>;
    static bool Capture(std::vector<uint8_t>& out, std::string* error = nullptr);
    static bool Validate(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
    static bool Restore(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
    static bool Decode(const std::vector<uint8_t>& bytes, LoaderFileIo::State& state,
                       std::string* error = nullptr);
    // Typed component codec for a composed service snapshot. Unlike Capture/
    // Restore, this includes Sound/Music destinations and their owner tickets.
    // The composition must validate/capture those owners too.
    static bool Encode(const LoaderFileIo::State& state, std::vector<uint8_t>& out,
                       std::string* error = nullptr);
    // Read-only legacy import of ordinary file handles from the game's own
    // StreamSystem/FileStream objects. No IOP RAM or transport records are
    // inspected. Pending reads and unidentified files are explicit errors;
    // music's standalone .mb handles still belong to Audio's migration.
    static bool MigrateFiles(uint32_t stream_system, LoaderFileIo::State& state,
                             std::string* error = nullptr);
    static bool MigrateFiles(uint32_t stream_system, const ReadMemory& read,
                             LoaderFileIo::State& state, std::string* error = nullptr);
    // A legacy asynchronous ring window still in flight on a FileStream object:
    // the one fill the retained module was serving, recovered from the title's
    // own words (fs+0xc fill position, fs+0x14 callback stream base/capacity,
    // fs+0x10/0x18 staged destination/size; TR game/filestream.cpp ring rule,
    // NTSC FUN_002aac80 0x2aacf0-0x2ab128). A direct window stages nothing on
    // the EE side and remains an explicit refusal.
    struct PendingWindow {
        uint32_t fs = 0, ring_base = 0, staged_dest = 0, staged_size = 0, fill = 0, offset = 0;
        int32_t handle = -1;
        uint64_t position = 0;   // byte offset of the fill from the file's first sector
    };
    // Same import, admitting pending ring windows into `pending` and the owned
    // window table of `state` (the loader's poll of the re-asked window).
    static bool MigrateFiles(uint32_t stream_system, const ReadMemory& read, LoaderFileIo::State& state,
                             std::vector<PendingWindow>& pending, std::string* error = nullptr);
    // Re-issue the recovered fills through the owned scheduler after
    // LoaderFileIo::RestoreState(state): the bytes come from the mounted
    // medium, the deadline from the owned read timing at `tick`; the title's
    // own completion copy still lands the staged window. All or nothing.
    using ReadDisc = std::function<bool(uint32_t lba, uint64_t byte_offset, uint32_t size, uint8_t* out)>;
    static bool ImportPending(const std::vector<PendingWindow>& pending, const LoaderFileIo::State& state,
                              const ReadDisc& read_disc, uint64_t tick, std::string* error = nullptr);
};
