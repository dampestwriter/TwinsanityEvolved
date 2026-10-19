#include "games/slus_20909_crash_twinsanity/hle/saves/save_state_codec.h"

#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/input/snapshot_bytes.h"

namespace title {
namespace {
bool Operation(SnapshotBytes& io, SaveStorage::Operation& operation) {
    int32_t value = static_cast<int32_t>(operation);
    if (!io.I32(value)) return false;
    operation = static_cast<SaveStorage::Operation>(value);
    return true;
}
bool Result(SnapshotBytes& io, SaveStorage::Result& result) {
    return io.I32(result.succeeded) && io.I32(result.step) && io.I32(result.error);
}
bool Entry(SnapshotBytes& io, SaveStorage::FileEntry& entry) {
    return io.Raw(entry.created.data(), entry.created.size()) &&
        io.Raw(entry.modified.data(), entry.modified.size()) && io.U32(entry.size) &&
        io.U16(entry.attributes) && io.U16(entry.reserved0) &&
        io.U32(entry.reserved1[0]) && io.U32(entry.reserved1[1]) &&
        io.Raw(entry.name.data(), entry.name.size());
}
bool Fields(SnapshotBytes& io, SaveBindings::Snapshot& snapshot) {
    uint32_t tag = SaveStateCodec::kTag;
    auto& state = snapshot.storage;
    if (!io.U32(tag) || tag != SaveStateCodec::kTag || !io.U32(snapshot.version) ||
        snapshot.version != SaveStateCodec::kVersion || !io.U32(state.version) ||
        state.version != 1 || !io.Boolean(state.initialised) ||
        !io.Text(state.region, 7) || !io.Text(state.product, 15) ||
        !io.U64(state.generation) || !io.U64(state.sequence) || !io.U64(state.last_boundary) ||
        !Operation(io, state.active) || !io.U32(state.progress)) return false;
    auto& request = state.pending;
    uint32_t port = request.port, slot = request.slot;
    if (!Operation(io, request.operation) || !io.U32(port) || !io.U32(slot) ||
        !io.Text(request.save, 31) || !io.Text(request.file, 31) ||
        !io.U32(request.size) || !io.Bytes(request.bytes, 0)) return false;
    request.port = port;
    request.slot = slot;
    for (auto& result : state.results) if (!Result(io, result)) return false;
    for (auto& info : state.info)
        if (!io.I32(info.type) || !io.I32(info.free_kilobytes) || !io.I32(info.formatted)) return false;
    bool completed = state.completion.has_value();
    if (!io.Boolean(completed)) return false;
    if (completed) {
        if (!state.completion) state.completion.emplace();
        auto& completion = *state.completion;
        if (!io.U64(completion.generation) || !io.U64(completion.sequence) ||
            !Operation(io, completion.operation) || !Result(io, completion.result) ||
            !io.I32(completion.value) || !Entry(io, completion.entry) ||
            !io.Boolean(completion.entry_present) || !io.Boolean(completion.committed) ||
            !io.Bytes(completion.bytes, SaveStorage::kMaxBytes)) return false;
    } else {
        state.completion.reset();
    }
    auto& out = snapshot.outputs;
    return io.U32(out.value) && io.U32(out.entry) && io.U32(out.data) && io.U32(out.size) && io.Done();
}
bool BadEncoding(std::string* error) {
    if (error) *error = "SaveStorage: invalid snapshot payload length, version or field encoding";
    return false;
}
}

bool SaveStateCodec::Encode(const SaveBindings::Snapshot& snapshot, std::vector<uint8_t>& out, std::string* error) {
    if (!SaveBindings::ValidateSnapshot(snapshot, error)) return false;
    auto value = snapshot;
    std::vector<uint8_t> bytes;
    SnapshotBytes writer(bytes, kMaxPayloadBytes);
    if (!Fields(writer, value)) return BadEncoding(error);
    out = std::move(bytes);
    return true;
}
bool SaveStateCodec::Decode(const std::vector<uint8_t>& bytes, SaveBindings::Snapshot& out, std::string* error) {
    if (bytes.size() > kMaxPayloadBytes) return BadEncoding(error);
    SaveBindings::Snapshot value;
    SnapshotBytes reader(bytes);
    if (!Fields(reader, value)) return BadEncoding(error);
    if (!SaveBindings::ValidateSnapshot(value, error)) return false;
    out = std::move(value);
    return true;
}
bool SaveStateCodec::Validate(const std::vector<uint8_t>& bytes, std::string* error) {
    SaveBindings::Snapshot value;
    return Decode(bytes, value, error);
}

}  // namespace title
