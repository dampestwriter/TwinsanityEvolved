#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"

#include <utility>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/stream/stream_library.h"

#include "engine/ee/recomp_abi/host_app/memory.h"

namespace {
bool Extent(uint32_t address, uint32_t size) {
    return address >= 0x10000 && address < 0x02000000 && size <= 0x02000000 - address;
}
bool Value(const StreamState::ReadMemory& read, uint32_t address, uint32_t size, uint32_t& value) {
    uint8_t bytes[4]{};
    if (!read || size > sizeof(bytes) || !Extent(address, size) || !read(address, bytes, size)) return false;
    value = 0;
    for (uint32_t i = 0; i < size; ++i) value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
    return true;
}
bool Text(const StreamState::ReadMemory& read, uint32_t object, std::string& value) {
    uint32_t pointer = 0, length = 0, capacity = 0;
    if (!Extent(object, 12) || !Value(read, object, 4, pointer) ||
        !Value(read, object + 4, 4, length) || !Value(read, object + 8, 4, capacity) ||
        !length || length > LoaderFileIo::kMaxPath || capacity <= length ||
        !Extent(pointer, length + 1)) return false;
    std::vector<uint8_t> bytes(length + 1);
    if (!read(pointer, bytes.data(), bytes.size()) || bytes.back() != 0) return false;
    for (uint32_t i = 0; i < length; ++i) if (!bytes[i]) return false;
    value.assign(reinterpret_cast<const char*>(bytes.data()), length);
    return true;
}
bool AddFile(LoaderFileIo::State& state, int32_t handle, const std::string& path) {
    LoaderFileIo::File file;
    if (!LoaderFileIo::ResolveFile(path, file)) return false;
    const auto existing = state.files.find(handle);
    if (existing != state.files.end()) {
        return existing->second.lba == file.lba && existing->second.size == file.size;
    }
    state.files.emplace(handle, std::move(file));
    // A legacy open was registered with the shared module. Preserve that
    // obligation until the retained audio owner/module is removed; ordinary
    // future reads do not use it. Never silently leak or fake the old close.
    state.streams.files.emplace(handle, StreamLibrary::FileState{StreamLibrary::IsSampleFile(path), true});
    return true;
}
}

bool StreamState::MigrateFiles(uint32_t system, LoaderFileIo::State& out, std::string* error) {
    return MigrateFiles(system, [](uint32_t address, uint8_t* bytes, size_t size) {
        if (size > UINT32_MAX || !Extent(address, static_cast<uint32_t>(size))) return false;
        for (size_t i = 0; i < size; ++i) bytes[i] = memory::read<uint8_t>(address + static_cast<uint32_t>(i));
        return true;
    }, out, error);
}

bool StreamState::MigrateFiles(uint32_t system, const ReadMemory& read, LoaderFileIo::State& out,
                               std::string* error) {
    std::vector<PendingWindow> pending;
    LoaderFileIo::State state;
    if (!MigrateFiles(system, read, state, pending, error)) return false;
    if (!pending.empty()) {
        if (error) *error = "legacy file read still pending";
        return false;
    }
    out = std::move(state);
    return true;
}

namespace {
// The staged ring window of a FileStream with its read flag up. Only the
// "miss" shape is recoverable from the EE side: fs+0xc is the fill position,
// fs+0x10/fs+0x18 the window the completion copy lands, the callback stream
// at fs+0x14 holds base/cursor/capacity; the fill is min(eof - pos, cap).
bool PendingRingWindow(const StreamState::ReadMemory& read, uint32_t fs, uint32_t flags, int32_t handle,
                       const LoaderFileIo::File& file, StreamState::PendingWindow& out, const char*& why) {
    uint32_t fill_start = 0, staged_dest = 0, staged_size = 0, cb = 0, seek = 0;
    if (!(flags & LoaderFileIo::kFlagRingValid)) { why = "legacy direct read record is held by the retained module only"; return false; }
    if (!Value(read, fs + LoaderFileIo::kFsRingStart, 4, fill_start) ||
        !Value(read, fs + LoaderFileIo::kFsDest, 4, staged_dest) ||
        !Value(read, fs + LoaderFileIo::kFsSize, 4, staged_size) ||
        !Value(read, fs + LoaderFileIo::kFsCbObj, 4, cb) ||
        !Value(read, fs + LoaderFileIo::kFsSeek, 4, seek)) { why = "unreadable pending ring window"; return false; }
    uint32_t base = 0, cursor = 0, cap = 0;
    if (!Extent(cb, 0x14) || !Value(read, cb + LoaderFileIo::kCbBase, 4, base) ||
        !Value(read, cb + LoaderFileIo::kCbCursor, 4, cursor) ||
        !Value(read, cb + LoaderFileIo::kCbCap, 4, cap)) { why = "pending ring window has no callback stream"; return false; }
    if (!staged_dest || !staged_size || staged_size >= cap || !Extent(base, cap) ||
        !Extent(staged_dest, staged_size) || cursor != base || fill_start < seek ||
        fill_start >= file.size || !file.found) { why = "pending ring window staging is not the fill the module served"; return false; }
    out.fs = fs; out.ring_base = base; out.staged_dest = staged_dest; out.staged_size = staged_size;
    out.handle = handle; out.position = fill_start; out.offset = fill_start - seek;
    const uint64_t remaining = file.size - fill_start;
    out.fill = static_cast<uint32_t>(remaining < cap ? remaining : cap);
    if (out.fill < staged_size) { why = "pending ring window exceeds its file"; return false; }
    return true;
}
}

bool StreamState::MigrateFiles(uint32_t system, const ReadMemory& read, LoaderFileIo::State& out,
                               std::vector<PendingWindow>& pending, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!StreamLibrary::Enabled()) return fail("file migration requires stream file selection");
    if (!read || !Extent(system, 0x1c)) return fail("invalid StreamSystem or memory provider");
    // TR game/filestream.h (0x1c / 0x48); NTSC manager constructor
    // 0x2add88 and file-object adapters independently checked in the module
    // self-oracle fixture. PoolSlotUsed == -1, a free tail is -2.
    uint32_t capacity = 0, used = 0, links = 0, streams = 0;
    if (!Value(read, system, 2, capacity) || !Value(read, system + 4, 2, used) ||
        !Value(read, system + 8, 4, links) || !Value(read, system + 12, 4, streams))
        return fail("unreadable StreamSystem pool");
    if (capacity > 0x7fff || used > capacity ||
        (capacity && (!Extent(links, capacity * 2u) || !Extent(streams, capacity * 4u))))
        return fail("invalid StreamSystem pool");
    LoaderFileIo::State state;
    state.streams.enabled = true;
    std::vector<PendingWindow> windows;
    uint32_t observed = 0;
    for (uint32_t slot = 0; slot < capacity; ++slot) {
        uint32_t link = 0;
        if (!Value(read, links + slot * 2, 2, link)) return fail("unreadable StreamSystem link");
        if (link != 0xffff) continue;
        ++observed;
        uint32_t fs = 0, flags = 0, handle_word = 0;
        if (!Value(read, streams + slot * 4, 4, fs) || !Extent(fs, 0x48) ||
            !Value(read, fs, 4, flags) || (flags & 0xff) != slot ||
            !Value(read, fs + LoaderFileIo::kFsHandle, 4, handle_word))
            return fail("invalid FileStream object or slot");
        const int32_t handle = static_cast<int32_t>(handle_word);
        const bool read_pending = (flags & LoaderFileIo::kFlagReadPending) != 0;
        if (read_pending && handle < 0) return fail("legacy file read pending on a closed handle");
        const uint32_t archive = (flags >> 8) & 0xff;
        if (archive > 4) return fail("invalid archive lifecycle");
        std::string path;
        if (archive >= 3) {
            uint32_t archive_file = 0;
            if (!Value(read, fs + 0x40, 4, archive_file) || archive_file > INT32_MAX ||
                handle != static_cast<int32_t>(archive_file) || !Text(read, fs + 0x34, path))
                return fail("archive backing-file identity unavailable");
            path += ".BD";
        } else if (handle < 0) {
            continue;
        } else if (!Text(read, fs + 0x1c, path)) {
            return fail("open file identity unavailable");
        }
        if (!AddFile(state, handle, path)) return fail("file identity conflicts or does not exist on mounted media");
        if (read_pending) {
            PendingWindow window;
            const char* why = nullptr;
            if (!PendingRingWindow(read, fs, flags, handle, state.files.at(handle), window, why)) return fail(why);
            for (const auto& other : windows) if (other.fs == fs) return fail("duplicate pending FileStream object");
            windows.push_back(window);
            state.windows[fs] = LoaderFileIo::Window{window.staged_dest, window.staged_size, window.offset};
        }
    }
    if (observed != used) return fail("StreamSystem used-slot count mismatch");
    if (!LoaderFileIo::ValidateState(state, error)) return false;
    out = std::move(state);
    pending = std::move(windows);
    return true;
}

bool StreamState::ImportPending(const std::vector<PendingWindow>& pending, const LoaderFileIo::State& state,
                                const ReadDisc& read_disc, uint64_t tick, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!read_disc) return fail("pending import requires the mounted medium");
    auto* schedule = StreamLibrary::Scheduling();
    if (!schedule) return fail("pending import requires the owned file scheduler");
    std::vector<std::pair<FileSchedule::Payload, std::pair<uint32_t, uint32_t>>> staged;
    for (const auto& window : pending) {
        const auto file = state.files.find(window.handle);
        const auto owned = state.streams.files.find(window.handle);
        if (file == state.files.end() || owned == state.streams.files.end() || !file->second.found ||
            !StreamLibrary::FileOwned(window.handle) || owned->second.sample)
            return fail("pending window has no owned ordinary file");
        if (!window.fill || window.position >= file->second.size || window.fill > file->second.size - window.position ||
            !Extent(window.ring_base, window.fill) || window.staged_size > window.fill ||
            state.windows.find(window.fs) == state.windows.end() || schedule->InFlight(window.fs))
            return fail("pending window disagrees with its restored file state");
        FileSchedule::Payload payload;
        payload.object = window.fs; payload.handle = window.handle; payload.file_offset = window.position;
        payload.destination = window.ring_base; payload.bytes.resize(window.fill);
        if (!read_disc(file->second.lba, window.position, window.fill, payload.bytes.data()))
            return fail("pending window bytes are not readable from the mounted medium");
        const auto skew = static_cast<uint32_t>(window.position % 2048);
        const auto sectors = (skew + window.fill + 2047) / 2048;
        staged.emplace_back(std::move(payload),
            std::make_pair(file->second.lba + static_cast<uint32_t>(window.position / 2048), sectors));
    }
    // Validated and read before the first submit: a refused window leaves no partial import.
    for (auto& entry : staged)
        if (!StreamLibrary::SubmitFile(std::move(entry.first), entry.second.first, entry.second.second, tick))
            return fail("owned scheduler refused the pending window");
    return true;
}
