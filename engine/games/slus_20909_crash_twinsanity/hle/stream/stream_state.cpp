#include "games/slus_20909_crash_twinsanity/hle/stream/stream_state.h"

#include <cstring>
#include <limits>
#include <utility>

namespace {
constexpr uint32_t kVersion = 4, kMaxRecords = 65536, kMaxBytes = 72 * 1024 * 1024;
void Put(std::vector<uint8_t>& out, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(value >> shift));
}
void Put64(std::vector<uint8_t>& out, uint64_t value) {
    Put(out, static_cast<uint32_t>(value));
    Put(out, static_cast<uint32_t>(value >> 32));
}
void PutReal(std::vector<uint8_t>& out, double value) {
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value), "snapshot doubles require 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    Put64(out, bits);
}
void PutSchedule(std::vector<uint8_t>& out, const FileSchedule::State& state) {
    Put(out, state.deferred); Put(out, state.mode); Put(out, state.visibility);
    PutReal(out, state.scale); PutReal(out, state.poll_scale);
    Put64(out, state.tick); Put64(out, state.next_order);
    Put(out, state.service_time); PutReal(out, state.service.settled_ms);
    Put64(out, state.service.sequence);
    PutReal(out, state.timing.busy_ms); PutReal(out, state.timing.last_end_ms);
    Put(out, state.timing.head_lba);
    Put64(out, state.budget.bytes); Put64(out, state.budget.epoch);
    Put64(out, state.budget.remaining); Put(out, state.budget.primed);
    Put(out, static_cast<uint32_t>(state.note)); Put(out, state.noted_object);
    Put(out, static_cast<uint32_t>(state.reads.size()));
    Put(out, static_cast<uint32_t>(state.arrivals.size()));
    for (const auto& pair : state.reads) {
        const auto& read = pair.second;
        const auto& payload = *read.payload;
        Put(out, payload.object); Put(out, payload.destination);
        Put(out, payload.ring_base); Put(out, payload.ring_cursor);
        Put(out, static_cast<uint32_t>(payload.handle)); Put64(out, payload.file_offset);
        Put(out, read.lba); Put(out, read.sectors);
        Put64(out, read.order); Put64(out, read.due); PutReal(out, read.end_ms);
        Put(out, static_cast<uint32_t>(read.phase));
        Put(out, read.accepted); Put(out, static_cast<uint32_t>(payload.kind));
        Put(out, payload.sound); Put(out, payload.stream);
        Put64(out, payload.generation); Put64(out, payload.sequence);
        Put(out, static_cast<uint32_t>(payload.bytes.size()));
        out.insert(out.end(), payload.bytes.begin(), payload.bytes.end());
    }
    for (const auto order : state.arrivals) Put64(out, order);
}
bool GetSchedule(const std::vector<uint8_t>& bytes, size_t& cursor, FileSchedule::State& state,
                 uint32_t version) {
    const auto get = [&](uint32_t& value) {
        if (bytes.size() - cursor < 4) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8)
            value |= static_cast<uint32_t>(bytes[cursor++]) << shift;
        return true;
    };
    const auto get64 = [&](uint64_t& value) {
        uint32_t lo = 0, hi = 0;
        if (!get(lo) || !get(hi)) return false;
        value = static_cast<uint64_t>(hi) << 32 | lo;
        return true;
    };
    const auto real = [&](double& value) {
        uint64_t bits = 0;
        if (!get64(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    };
    uint32_t deferred = 0, primed = 0, note = 0, reads = 0, arrivals = 0;
    if (!get(deferred) || deferred > 1 || !get(state.mode) || !get(state.visibility) ||
        !real(state.scale) || !real(state.poll_scale) || !get64(state.tick) || !get64(state.next_order)) return false;
    if (version >= 4) {
        uint32_t service_time = 0;
        if (!get(service_time) || service_time > 1 ||
            !real(state.service.settled_ms) || !get64(state.service.sequence)) return false;
        state.service_time = service_time != 0;
    }
    if (!real(state.timing.busy_ms) || !real(state.timing.last_end_ms) || !get(state.timing.head_lba) ||
        !get64(state.budget.bytes) || !get64(state.budget.epoch) || !get64(state.budget.remaining) ||
        !get(primed) || primed > 1 || !get(note) || note > 2 || !get(state.noted_object) ||
        !get(reads) || reads > kMaxRecords || !get(arrivals) || arrivals > reads) return false;
    state.deferred = deferred != 0;
    state.budget.primed = primed != 0;
    state.note = static_cast<FileSchedule::PollOutcome>(note);
    for (uint32_t i = 0; i < reads; ++i) {
        auto payload = std::make_shared<FileSchedule::Payload>();
        FileSchedule::Read read;
        uint32_t handle = 0, phase = 0, size = 0;
        if (!get(payload->object) || !get(payload->destination) ||
            !get(payload->ring_base) || !get(payload->ring_cursor) ||
            !get(handle) || handle > INT32_MAX || !get64(payload->file_offset) ||
            !get(read.lba) || !get(read.sectors) || !get64(read.order) || !get64(read.due) ||
            !real(read.end_ms) || !get(phase) || phase > 3) return false;
        if (version >= 3) {
            uint32_t accepted = 0, kind = 0;
            if (!get(accepted) || accepted > 1 || !get(kind) || kind > 2 ||
                !get(payload->sound) || !get(payload->stream) ||
                !get64(payload->generation) || !get64(payload->sequence)) return false;
            read.accepted = accepted != 0;
            payload->kind = static_cast<FileSchedule::Destination>(kind);
        }
        if (!get(size) || size > bytes.size() - cursor) return false;
        payload->handle = static_cast<int32_t>(handle);
        payload->bytes.assign(bytes.begin() + cursor, bytes.begin() + cursor + size);
        cursor += size;
        read.phase = static_cast<FileSchedule::Phase>(phase);
        read.payload = payload;
        if (!state.reads.emplace(payload->object, std::move(read)).second) return false;
    }
    for (uint32_t i = 0; i < arrivals; ++i) {
        uint64_t order = 0;
        if (!get64(order)) return false;
        state.arrivals.push_back(order);
    }
    return true;
}
bool Error(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
bool FilesOnly(const LoaderFileIo::State& state, std::string* error) {
    for (const auto& read : state.streams.schedule.reads)
        if (read.second.payload->kind != FileSchedule::Destination::Memory)
            return Error(error, "sound requests require composed Audio state");
    return true;
}
}

bool StreamState::Capture(std::vector<uint8_t>& out, std::string* error) {
    LoaderFileIo::State state;
    if (!LoaderFileIo::CaptureState(state)) return Error(error, "file deliveries still pending");
    if (!FilesOnly(state, error)) return false;
    return Encode(state, out, error);
}
bool StreamState::Encode(const LoaderFileIo::State& state, std::vector<uint8_t>& out, std::string* error) {
    if (!LoaderFileIo::ValidateState(state, error)) return false;
    std::vector<uint8_t> encoded;
    Put(encoded, kVersion);
    Put(encoded, state.streams.enabled);
    Put(encoded, static_cast<uint32_t>(state.last_handle));
    Put(encoded, static_cast<uint32_t>(state.files.size()));
    Put(encoded, static_cast<uint32_t>(state.windows.size()));
    Put(encoded, static_cast<uint32_t>(state.streams.files.size()));
    Put(encoded, static_cast<uint32_t>(state.streams.completed_reads.size()));
    Put(encoded, static_cast<uint32_t>(state.streams.audio_reads.size()));
    for (const auto& entry : state.files) {
        Put(encoded, static_cast<uint32_t>(entry.first));
        Put(encoded, entry.second.lba); Put(encoded, entry.second.size); Put(encoded, entry.second.found);
        Put(encoded, static_cast<uint32_t>(entry.second.path.size()));
        encoded.insert(encoded.end(), entry.second.path.begin(), entry.second.path.end());
    }
    for (const auto& entry : state.windows) {
        Put(encoded, entry.first); Put(encoded, entry.second.dest);
        Put(encoded, entry.second.size); Put(encoded, entry.second.offset);
    }
    for (const auto& entry : state.streams.files) {
        Put(encoded, static_cast<uint32_t>(entry.first));
        Put(encoded, entry.second.sample); Put(encoded, entry.second.registered);
    }
    for (const auto* reads : {&state.streams.completed_reads, &state.streams.audio_reads})
        for (const auto& entry : *reads) { Put(encoded, entry.first); Put(encoded, static_cast<uint32_t>(entry.second)); }
    Put(encoded, state.streams.has_schedule);
    if (state.streams.has_schedule) PutSchedule(encoded, state.streams.schedule);
    if (encoded.size() > kMaxBytes) return Error(error, "oversized file-state payload");
    out = std::move(encoded);
    return true;
}

bool StreamState::Decode(const std::vector<uint8_t>& bytes, LoaderFileIo::State& out, std::string* error) {
    if (bytes.size() > kMaxBytes) return Error(error, "oversized file-state payload");
    size_t cursor = 0;
    const auto get = [&](uint32_t& value) {
        if (bytes.size() - cursor < 4) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8)
            value |= static_cast<uint32_t>(bytes[cursor++]) << shift;
        return true;
    };
    uint32_t version = 0, enabled = 0, last = 0, files = 0, windows = 0, owners = 0, completed = 0, audio = 0;
    if (!get(version) || version < 1 || version > kVersion || !get(enabled) || enabled > 1 || !get(last) ||
        !get(files) || files > kMaxRecords || !get(windows) || windows > kMaxRecords ||
        !get(owners) || owners > kMaxRecords || !get(completed) || completed > kMaxRecords ||
        !get(audio) || audio > kMaxRecords) return Error(error, "invalid file-state header");
    LoaderFileIo::State state;
    state.last_handle = static_cast<int32_t>(last);
    state.streams.enabled = enabled != 0;
    for (uint32_t i = 0; i < files; ++i) {
        uint32_t handle = 0, found = 0, length = 0;
        LoaderFileIo::File file;
        if (!get(handle) || handle > INT32_MAX || !get(file.lba) || !get(file.size) ||
            !get(found) || found > 1 || !get(length) || length == 0 ||
            length > LoaderFileIo::kMaxPath || length > bytes.size() - cursor)
            return Error(error, "invalid saved file entry");
        file.found = found != 0;
        file.path.assign(reinterpret_cast<const char*>(bytes.data() + cursor), length);
        cursor += length;
        if (!state.files.emplace(static_cast<int32_t>(handle), std::move(file)).second)
            return Error(error, "duplicate saved file handle");
    }
    for (uint32_t i = 0; i < windows; ++i) {
        uint32_t object = 0;
        LoaderFileIo::Window window;
        if (!get(object) || !get(window.dest) || !get(window.size) || !get(window.offset) ||
            !state.windows.emplace(object, window).second) return Error(error, "invalid saved read window");
    }
    for (uint32_t i = 0; i < owners; ++i) {
        uint32_t handle = 0, sample = 0, registered = 0;
        if (!get(handle) || handle > INT32_MAX || !get(sample) || sample > 1 ||
            !get(registered) || registered > 1 ||
            !state.streams.files.emplace(static_cast<int32_t>(handle),
                                        StreamLibrary::FileState{sample != 0, registered != 0}).second)
            return Error(error, "invalid file ownership entry");
    }
    for (const auto& pair : {std::make_pair(completed, &state.streams.completed_reads),
                            std::make_pair(audio, &state.streams.audio_reads)}) {
        for (uint32_t i = 0; i < pair.first; ++i) {
            uint32_t object = 0, handle = 0;
            if (!get(object) || !get(handle) || handle > INT32_MAX ||
                !pair.second->emplace(object, static_cast<int32_t>(handle)).second)
                return Error(error, "invalid read ownership entry");
        }
    }
    if (version >= 2) {
        uint32_t has_schedule = 0;
        if (!get(has_schedule) || has_schedule > 1)
            return Error(error, "invalid file scheduling marker");
        state.streams.has_schedule = has_schedule != 0;
        if (state.streams.enabled != state.streams.has_schedule)
            return Error(error, "selected file service requires its scheduling position");
        if (state.streams.has_schedule && !GetSchedule(bytes, cursor, state.streams.schedule, version))
            return Error(error, "invalid file scheduling payload");
    } else if (state.streams.enabled) {
        // Version 1 did not save the logical read position even at quiescence.
        // Loading it as a complete checkpoint would change future deadlines.
        return Error(error, "legacy file state requires logical timing migration");
    }
    if (cursor != bytes.size()) return Error(error, "trailing file-state bytes");
    if (!LoaderFileIo::ValidateState(state, error)) return false;
    out = std::move(state);
    return true;
}
bool StreamState::Validate(const std::vector<uint8_t>& bytes, std::string* error) {
    LoaderFileIo::State state;
    return Decode(bytes, state, error) && FilesOnly(state, error);
}
bool StreamState::Restore(const std::vector<uint8_t>& bytes, std::string* error) {
    LoaderFileIo::State state;
    return Decode(bytes, state, error) && FilesOnly(state, error) && LoaderFileIo::RestoreState(state, error);
}
