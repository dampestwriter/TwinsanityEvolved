#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_assets.h"

#include <cmath>
#include <cstring>
#include <set>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_state.h"

namespace title {
namespace {
bool Bad(std::string* error, const char* why) {
    if (error) *error = std::string("Sky state: ") + why;
    return false;
}
bool Object(uint32_t value) {
    return value && !(value & 3) && value < 0x02000000;
}
bool Power(uint32_t n) { return n >= 32 && n <= 128 && !(n & (n - 1)); }
template<class T> bool Value(const MaterialState::Read& read, uint32_t at, T& out) {
    return read && Object(at) && sizeof(T) <= 0x02000000 - at && read(at, &out, sizeof(T));
}
}

bool SkyAssets::ValidateSnapshot(const Snapshot& snapshot, std::string* error) {
    if (snapshot.models.size() > 4096 || snapshot.textures.size() > 4096)
        return Bad(error, "owner count exceeds limit");
    uint64_t bytes = 16;
    std::set<uint32_t> objects;
    uint32_t previous = 0;
    for (const auto& item : snapshot.models) {
        if (!Object(item.object) || item.object <= previous || !objects.insert(item.object).second ||
            item.model.parts.empty() || item.model.parts.size() > 64)
            return Bad(error, "invalid model identity, ordering or part count");
        previous = item.object;
        bytes += 12;
        size_t vertices = 0;
        for (const auto& mesh : item.model.parts) {
            if (mesh.strips.empty() || mesh.strips.size() > SkyGeometry::kMaxVertices / 3)
                return Bad(error, "invalid model strip count");
            bytes += 4;
            for (const auto& strip : mesh.strips) {
                if (strip.vertices.size() < 3 || strip.vertices.size() > 38)
                    return Bad(error, "invalid authored strip extent");
                vertices += strip.vertices.size();
                if (vertices > SkyGeometry::kMaxVertices) return Bad(error, "model vertex limit exceeded");
                bytes += 4 + strip.vertices.size() * 29;
                for (const auto& vertex : strip.vertices) {
                    for (float x : vertex.position) if (!std::isfinite(x)) return Bad(error, "nonfinite position");
                    for (size_t i = 0; i < vertex.stq.size(); ++i) {
                        uint32_t bits;
                        std::memcpy(&bits, &vertex.stq[i], sizeof(bits));
                        if (!std::isfinite(vertex.stq[i]) || uint8_t(bits) != vertex.colour[i])
                            return Bad(error, "authored STQ/colour bits disagree");
                    }
                }
            }
        }
    }
    previous = 0;
    for (const auto& item : snapshot.textures) {
        const auto& image = item.image;
        if (!Object(item.object) || item.object <= previous || !objects.insert(item.object).second ||
            !Power(image.width) || !Power(image.height) ||
            (image.buffer_width != 64 && image.buffer_width != 128) ||
            image.levels < 3 || image.levels > 5 || image.function > 3 ||
            image.rgba.size() != uint64_t(image.width) * image.height * 4)
            return Bad(error, "invalid texture identity, ordering or converted image");
        previous = item.object;
        bytes += 24 + image.rgba.size();
    }
    if (bytes > SkyState::kMaxBytes) return Bad(error, "section byte limit exceeded");
    if (error) error->clear();
    return true;
}

bool SkyAssets::Capture(Snapshot& out, std::string* error, bool unlinked_recording_open) const {
    Snapshot snapshot;
    uint64_t bytes = 16;
    for (const auto& [object, item] : models_) {
        if (item.lease.use_count() != 1 && !unlinked_recording_open)
            return Bad(error, "model still leased by unfinished work");
        bytes += 12;
        for (const auto& mesh : item.lease->parts) {
            bytes += 4;
            for (const auto& strip : mesh.strips) bytes += 4 + strip.vertices.size() * 29;
        }
        if (bytes > SkyState::kMaxBytes) return Bad(error, "section byte limit exceeded");
        snapshot.models.push_back({object, item.asset_id, *item.lease});
    }
    for (const auto& [object, item] : textures_) {
        if (item.lease.use_count() != 1 && !unlinked_recording_open)
            return Bad(error, "texture still leased by unfinished work");
        bytes += 24 + item.lease->rgba.size();
        if (bytes > SkyState::kMaxBytes) return Bad(error, "section byte limit exceeded");
        snapshot.textures.push_back({object, item.asset_id, *item.lease});
    }
    if (!ValidateSnapshot(snapshot, error)) return false;
    out = std::move(snapshot);
    return true;
}

bool SkyAssets::Restore(const Snapshot& snapshot, std::string* error) {
    if (!models_.empty() || !textures_.empty()) return Bad(error, "restore requires a fresh owner");
    if (!ValidateSnapshot(snapshot, error)) return false;
    decltype(models_) models;
    decltype(textures_) textures;
    for (const auto& item : snapshot.models)
        models.emplace(item.object, ModelEntry{item.asset_id, std::make_shared<Model>(item.model)});
    for (const auto& item : snapshot.textures)
        textures.emplace(item.object, TextureEntry{item.asset_id, std::make_shared<SkyTexture::Image>(item.image)});
    models_.swap(models);
    textures_.swap(textures);
    return true;
}

bool SkyAssets::Preflight(const Snapshot& snapshot, const MaterialState::Read& read, std::string* error) {
    if (!ValidateSnapshot(snapshot, error)) return false;
    for (const auto& item : snapshot.models) {
        std::array<uint32_t, 7> source{};
        if (!Value(read, item.object, source) || source[1] != item.asset_id ||
            source[2] != item.model.parts.size() || !Object(source[3]) ||
            source[3] > 0x02000000 - source[2] * 4)
            return Bad(error, "incoming model identity/parts differ");
        for (size_t part = 0; part < item.model.parts.size(); ++part) {
            size_t vertices = 0;
            for (const auto& strip : item.model.parts[part].strips) vertices += strip.vertices.size();
            uint32_t declared = 0;
            if (!Value(read, source[3] + uint32_t(part) * 4, declared) || declared != vertices)
                return Bad(error, "incoming model vertex count differs");
        }
    }
    for (const auto& item : snapshot.textures) {
        // TR GameTexture: ResourceHeader + size + Texture. Its mutable transfer,
        // slot and context fields are not sampled, decoded or reconstructed.
        std::array<uint32_t, 4> source{};
        std::array<uint8_t, 0x54> header{};
        if (!Value(read, item.object, source) || item.object > 0x02000000 - 0x60 ||
            !read(item.object + 12, header.data(), header.size()) || source[1] != item.asset_id)
            return Bad(error, "incoming texture identity differs");
        const auto& image = item.image;
        uint32_t width = 0;
        std::memcpy(&width, header.data() + 0x2c, 4);
        const uint32_t wp = header[4] | uint32_t(header[5]) << 8;
        const uint32_t hp = header[6] | uint32_t(header[7]) << 8;
        if (wp < 5 || wp > 7 || hp < 5 || hp > 7 || image.width != (1u << wp) ||
            image.height != (1u << hp) || width > 2 || image.buffer_width != width * 64 ||
            header[8] != image.levels || header[9] != 0x13 || header[10] ||
            header[11] != uint8_t(image.alpha) || header[13] != image.function)
            return Bad(error, "incoming texture descriptor differs");
    }
    return true;
}

}  // namespace title
