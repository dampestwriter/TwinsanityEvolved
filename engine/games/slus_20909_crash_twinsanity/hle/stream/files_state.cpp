#include "games/slus_20909_crash_twinsanity/hle/stream/files.h"

#include <limits>
#include <utility>

#include "engine/cdvd/iso_fs.h"
#include "games/slus_20909_crash_twinsanity/hle/scheduler/state_bytes.h"

namespace title {
namespace {
bool Fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
void PutText(std::vector<uint8_t>& out, const std::string& text) {
    StateBytes::Put(out, static_cast<uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}
bool GetText(StateBytes& in, std::string& text, uint32_t limit) {
    uint32_t length = 0;
    std::vector<uint8_t> bytes;
    if (!in.Get(length) || length > limit || !in.GetBytes(length, bytes)) return false;
    text.assign(bytes.begin(), bytes.end());
    return true;
}
}

Files::State Files::Snapshot() const {
    State state;
    state.image_bytes = disc_.ImageBytes(); state.volume_sectors = disc_.VolumeSectors();
    state.root_location = disc_.RootLba(); state.root_bytes = disc_.RootSize();
    state.volume_id = disc_.VolumeId();
    for (const auto& slot : slots_) if (slot.generation) state.files.push_back(slot.file);
    return state;
}

bool Files::Validate(const State& state, std::string* error) const {
    if (!disc_.IsOpen() || state.image_bytes != disc_.ImageBytes() ||
        state.volume_sectors != disc_.VolumeSectors() || state.root_location != disc_.RootLba() ||
        state.root_bytes != disc_.RootSize() || state.volume_id != disc_.VolumeId())
        return Fail(error, "disc file state media identity differs");
    if (state.files.size() > kMaxFiles ||
        state.files.size() >= std::numeric_limits<uint64_t>::max() - next_generation_)
        return Fail(error, "disc file handle or generation limit");
    std::array<bool, kMaxFiles> seen{};
    for (const auto& file : state.files) {
        IsoFs::Entry entry;
        if (file.handle >= kMaxFiles || seen[file.handle] || !DiscPath(file.path) ||
            file.position > file.size || file.size > uint32_t(std::numeric_limits<int32_t>::max()) ||
            !disc_.Find(file.path, entry) || entry.directory || entry.lba != file.location ||
            entry.size != file.size ||
            uint64_t(file.location) * IsoFs::kSectorSize + file.size > disc_.ImageBytes())
            return Fail(error, "disc file descriptor, cursor or source extent differs");
        seen[file.handle] = true;
    }
    return true;
}

bool Files::Restore(const State& state, std::string* error) {
    if (!Validate(state, error)) return false;
    std::array<Slot, kMaxFiles> restored{};
    uint64_t generation = next_generation_;
    for (const auto& file : state.files) restored[file.handle] = {file, generation++};
    slots_ = std::move(restored);
    next_generation_ = generation;
    return true;
}

bool Files::Encode(std::vector<uint8_t>& bytes, std::string* error) const {
    const auto state = Snapshot();
    if (!Validate(state, error)) return false;
    std::vector<uint8_t> out;
    StateBytes::Put(out, 1);
    StateBytes::Put64(out, state.image_bytes); StateBytes::Put(out, state.volume_sectors);
    StateBytes::Put(out, state.root_location); StateBytes::Put(out, state.root_bytes);
    PutText(out, state.volume_id);
    StateBytes::Put(out, static_cast<uint32_t>(state.files.size()));
    for (const auto& file : state.files) {
        StateBytes::Put(out, file.handle); StateBytes::Put(out, file.location);
        StateBytes::Put(out, file.size); StateBytes::Put(out, file.position);
        PutText(out, file.path);
    }
    bytes = std::move(out);
    return true;
}

bool Files::Decode(const std::vector<uint8_t>& bytes, State& state, std::string* error) const {
    StateBytes in(bytes);
    uint32_t version = 0, count = 0;
    State out;
    if (!in.Get(version) || version != 1 || !in.Get64(out.image_bytes) ||
        !in.Get(out.volume_sectors) || !in.Get(out.root_location) || !in.Get(out.root_bytes) ||
        !GetText(in, out.volume_id, 32) || !in.Get(count) || count > kMaxFiles)
        return Fail(error, "invalid disc file state header");
    out.files.resize(count);
    for (auto& file : out.files) {
        if (!in.Get(file.handle) || !in.Get(file.location) || !in.Get(file.size) ||
            !in.Get(file.position) || !GetText(in, file.path, kMaxPath - 1))
            return Fail(error, "truncated disc file descriptor");
    }
    if (in.Remaining()) return Fail(error, "trailing disc file state bytes");
    if (!Validate(out, error)) return false;
    state = std::move(out);
    return true;
}

} // namespace title
