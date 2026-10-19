#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_state.h"

#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_assets_codec.h"

namespace title {
namespace {
bool Refuse(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
}
bool ShadowState::Encode(const ShadowLibrary::Snapshot& snapshot, std::vector<uint8_t>& out) {
    std::vector<uint8_t> bytes{1, uint8_t(snapshot.selected), 0, 0};
    if (snapshot.selected) {
        std::vector<uint8_t> assets;
        if (!ShadowAssetsCodec::Encode(snapshot.assets, assets)) return false;
        bytes.insert(bytes.end(), assets.begin(), assets.end());
    } else if (!snapshot.assets.assets.empty() || snapshot.assets.next_generation != 1 ||
               snapshot.assets.version != 1) return false;
    if (bytes.size() > ps2r::savestate::kMaxSectionPayload) return false;
    out = std::move(bytes);
    return true;
}
bool ShadowState::Decode(const std::vector<uint8_t>& bytes, ShadowLibrary::Snapshot& out) {
    if (bytes.size() < 4 || bytes.size() > ps2r::savestate::kMaxSectionPayload ||
        bytes[0] != 1 || bytes[1] > 1 || bytes[2] || bytes[3]) return false;
    ShadowLibrary::Snapshot snapshot;
    snapshot.selected = bytes[1] != 0;
    if (snapshot.selected) {
        const std::vector<uint8_t> payload(bytes.begin() + 4, bytes.end());
        if (!ShadowAssetsCodec::Decode(payload, snapshot.assets)) return false;
    } else if (bytes.size() != 4) return false;
    out = std::move(snapshot);
    return true;
}
bool ShadowState::Preflight(const std::vector<ps2r::savestate::Blob>& sections, bool selected,
                            ShadowLibrary::Snapshot& out, MaterialState::Read& incoming,
                            std::string* error) {
    using namespace ps2r::savestate;
    const Blob* state = nullptr;
    const Blob* ram = nullptr;
    for (const auto& blob : sections) {
        if (blob.tag == kTag) {
            if (state) return Refuse(error, "Duplicate SHDW section");
            state = &blob;
        }
        if (blob.tag == TAG_RAM) {
            if (ram) return Refuse(error, "Duplicate incoming RAM0 section");
            ram = &blob;
        }
    }
    ShadowLibrary::Snapshot snapshot;
    if (!state) {
        if (selected)
            return Refuse(error, "Selected Shadow state is missing; legacy idle/asset import is not established");
    } else if (state->version != kVersion || (state->flags & ~kSectionRequired) ||
               !Decode(state->payload, snapshot)) {
        return Refuse(error, "Invalid SHDW version, flags or owned bytes");
    }
    if (snapshot.selected != selected)
        return Refuse(error, "SHDW selection differs from the active Shadow owner");
    if (selected && !(state->flags & kSectionRequired))
        return Refuse(error, "Selected SHDW must be required");
    if (!ram || ram->version != 1 || ram->payload.size() != 0x02000000)
        return Refuse(error, "Shadow requires complete incoming RAM0");
    const auto* data = ram->payload.data();
    const auto size = ram->payload.size();
    MaterialState::Read reader = [data, size](uint32_t at, void* target, size_t count) {
        if (!target || at > size || count > size - at) return false;
        std::memcpy(target, data + at, count);
        return true;
    };
    if (selected && (!ShadowAssets::ValidateSource(snapshot.assets, reader) ||
                     !ShadowAssets::ValidateConsumers(snapshot.assets, reader)))
        return Refuse(error, "SHDW identity/extent differs from incoming RAM0");
    out = std::move(snapshot);
    incoming = std::move(reader); // borrows image only for immediate owner Stage
    return true;
}
}  // namespace title
