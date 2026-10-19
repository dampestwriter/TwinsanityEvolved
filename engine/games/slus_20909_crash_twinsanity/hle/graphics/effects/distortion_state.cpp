#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_state.h"

#include <cstring>

namespace title {
namespace {
bool Refuse(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
template<class T> void Put(std::vector<uint8_t>& out, T value) {
    uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
template<class T> bool Get(const std::vector<uint8_t>& in, size_t& at, T& value) {
    if (in.size() - at < sizeof(T)) return false;
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}
bool EmptyModelState(const DistortionLibrary::Snapshot& snapshot) {
    return !snapshot.blocks.generation && !snapshot.blocks.pool && snapshot.blocks.next.empty() &&
        snapshot.blocks.version == 1 && !snapshot.settings.shader && !snapshot.settings.generation &&
        snapshot.settings.version == 1;
}
}
bool DistortionState::Encode(const DistortionLibrary::Snapshot& snapshot, std::vector<uint8_t>& out) {
    std::vector<uint8_t> bytes{1, uint8_t(snapshot.selected), 0, 0};
    if (snapshot.selected) {
        if (!DistortionBlocks::Validate(snapshot.blocks) || !DistortionSettings::Validate(snapshot.settings))
            return false;
        Put<uint32_t>(bytes, snapshot.blocks.pool);
        Put<uint64_t>(bytes, snapshot.blocks.generation);
        Put<uint32_t>(bytes, uint32_t(snapshot.blocks.next.size()));
        for (const auto& pair : snapshot.blocks.next) { Put<uint32_t>(bytes, pair.first); Put<uint32_t>(bytes, pair.second); }
        Put<uint32_t>(bytes, snapshot.settings.shader);
        Put<uint64_t>(bytes, snapshot.settings.generation);
        Put<uint64_t>(bytes, snapshot.settings.settings.attributes);
        Put<int16_t>(bytes, snapshot.settings.settings.lod_k);
        Put<int16_t>(bytes, snapshot.settings.settings.lod_l);
    } else if (!EmptyModelState(snapshot)) {
        return false;
    }
    if (bytes.size() > ps2r::savestate::kMaxSectionPayload) return false;
    out = std::move(bytes);
    return true;
}
bool DistortionState::Decode(const std::vector<uint8_t>& bytes, DistortionLibrary::Snapshot& out) {
    if (bytes.size() < 4 || bytes.size() > ps2r::savestate::kMaxSectionPayload ||
        bytes[0] != 1 || bytes[1] > 1 || bytes[2] || bytes[3]) return false;
    DistortionLibrary::Snapshot snapshot;
    snapshot.selected = bytes[1] != 0;
    if (!snapshot.selected) {
        if (bytes.size() != 4) return false;
        out = std::move(snapshot);
        return true;
    }
    size_t at = 4;
    uint32_t count = 0;
    if (!Get(bytes, at, snapshot.blocks.pool) || !Get(bytes, at, snapshot.blocks.generation) ||
        !Get(bytes, at, count) || count > 4096) return false;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t block = 0, next = 0;
        if (!Get(bytes, at, block) || !Get(bytes, at, next) || (i && block <= previous)) return false;
        snapshot.blocks.next.emplace(block, next);
        previous = block;
    }
    if (!Get(bytes, at, snapshot.settings.shader) || !Get(bytes, at, snapshot.settings.generation) ||
        !Get(bytes, at, snapshot.settings.settings.attributes) ||
        !Get(bytes, at, snapshot.settings.settings.lod_k) || !Get(bytes, at, snapshot.settings.settings.lod_l) ||
        at != bytes.size()) return false;
    if (!DistortionBlocks::Validate(snapshot.blocks) || !DistortionSettings::Validate(snapshot.settings))
        return false;
    out = std::move(snapshot);
    return true;
}
bool DistortionState::Preflight(const std::vector<ps2r::savestate::Blob>& sections, bool selected,
                                DistortionLibrary::Snapshot& out, MaterialState::Read& incoming,
                                std::string* error) {
    using namespace ps2r::savestate;
    const Blob* state = nullptr;
    const Blob* ram = nullptr;
    for (const auto& blob : sections) {
        if (blob.tag == kTag) {
            if (state) return Refuse(error, "Duplicate DSTN section");
            state = &blob;
        }
        if (blob.tag == TAG_RAM) {
            if (ram) return Refuse(error, "Duplicate incoming RAM0 section");
            ram = &blob;
        }
    }
    DistortionLibrary::Snapshot snapshot;
    if (!state) {
        if (selected)
            return Refuse(error, "Selected Distortion state is missing; legacy import is not established");
    } else if (state->version != kVersion || (state->flags & ~kSectionRequired) ||
               !Decode(state->payload, snapshot)) {
        return Refuse(error, "Invalid DSTN version, flags or owned bytes");
    }
    if (snapshot.selected != selected)
        return Refuse(error, "DSTN selection differs from the active Distortion owner");
    if (selected && !(state->flags & kSectionRequired))
        return Refuse(error, "Selected DSTN must be required");
    if (!ram || ram->version != 1 || ram->payload.size() != 0x02000000)
        return Refuse(error, "Distortion requires complete incoming RAM0");
    const auto* data = ram->payload.data();
    const auto size = ram->payload.size();
    MaterialState::Read reader = [data, size](uint32_t address, void* target, size_t count) {
        const auto at = address & 0x1fffffff;
        if (!target || at > size || count > size - at) return false;
        std::memcpy(target, data + at, count);
        return true;
    };
    if (selected && snapshot.settings.shader) {
        DistortionMaterial::Settings probe;
        if (DistortionMaterial::CaptureSettings(reader, snapshot.settings.shader, probe) !=
            DistortionMaterial::Result::Ok)
            return Refuse(error, "DSTN settings version names no retail material in incoming RAM0");
    }
    out = std::move(snapshot);
    incoming = std::move(reader); // borrows the image only for the immediate owner Stage
    return true;
}
}  // namespace title
