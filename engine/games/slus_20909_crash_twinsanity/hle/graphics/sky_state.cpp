#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_state.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace title {
namespace {
bool Bad(std::string* error, const char* why) {
    if (error) *error = std::string("Sky state: ") + why;
    return false;
}
void Put(std::vector<uint8_t>& out, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(uint8_t(n >> (i * 8)));
}
void Float(std::vector<uint8_t>& out, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    Put(out, bits);
}
}

struct SkyState::Reader {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    bool ok = true;
    size_t Left() const { return bytes.size() - at; }
    uint32_t Word() {
        if (!ok || Left() < 4) { ok = false; return 0; }
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[at++]) << (i * 8);
        return value;
    }
    float Float() {
        const auto bits = Word();
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
    uint8_t Byte() {
        if (!ok || !Left()) { ok = false; return 0; }
        return bytes[at++];
    }
    bool Raw(std::vector<uint8_t>& out, size_t size) {
        if (!ok || Left() < size) { ok = false; return false; }
        out.assign(bytes.begin() + at, bytes.begin() + at + size);
        at += size;
        return true;
    }
};

bool SkyState::Encode(const SkyAssets::Snapshot& snapshot, std::vector<uint8_t>& out, std::string* error) {
    if (!SkyAssets::ValidateSnapshot(snapshot, error)) return false;
    std::vector<uint8_t> bytes;
    Put(bytes, kTag); Put(bytes, kVersion);
    Put(bytes, uint32_t(snapshot.models.size())); Put(bytes, uint32_t(snapshot.textures.size()));
    for (const auto& item : snapshot.models) {
        Put(bytes, item.object); Put(bytes, item.asset_id); Put(bytes, uint32_t(item.model.parts.size()));
        for (const auto& mesh : item.model.parts) {
            Put(bytes, uint32_t(mesh.strips.size()));
            for (const auto& strip : mesh.strips) {
                Put(bytes, uint32_t(strip.vertices.size()));
                for (const auto& vertex : strip.vertices) {
                    for (float f : vertex.position) Float(bytes, f);
                    for (float f : vertex.stq) Float(bytes, f);
                    bytes.insert(bytes.end(), vertex.colour.begin(), vertex.colour.end());
                    bytes.push_back(uint8_t(vertex.restart));
                }
            }
        }
    }
    for (const auto& item : snapshot.textures) {
        const auto& image = item.image;
        Put(bytes, item.object); Put(bytes, item.asset_id);
        Put(bytes, image.width); Put(bytes, image.height); Put(bytes, image.buffer_width);
        bytes.push_back(image.levels); bytes.push_back(image.function);
        bytes.push_back(uint8_t(image.alpha)); bytes.push_back(0);
        bytes.insert(bytes.end(), image.rgba.begin(), image.rgba.end());
    }
    out = std::move(bytes);
    return true;
}

bool SkyState::Decode(const std::vector<uint8_t>& bytes, SkyAssets::Snapshot& out, std::string* error) {
    if (bytes.size() < 16 || bytes.size() > kMaxBytes) return Bad(error, "invalid payload extent");
    Reader read{bytes};
    if (read.Word() != kTag || read.Word() != kVersion) return Bad(error, "tag/version differs");
    const uint32_t models = read.Word(), textures = read.Word();
    if (models > 4096 || textures > 4096 || models > read.Left() / 107)
        return Bad(error, "invalid owner count");
    SkyAssets::Snapshot snapshot;
    for (uint32_t i = 0; i < models; ++i) {
        SkyAssets::SavedModel item;
        item.object = read.Word(); item.asset_id = read.Word();
        const auto parts = read.Word();
        if (!read.ok || !parts || parts > 64 || parts > read.Left() / 95)
            return Bad(error, "invalid model part count");
        size_t total = 0;
        for (uint32_t part = 0; part < parts; ++part) {
            SkyGeometry::Mesh mesh;
            const auto strips = read.Word();
            if (!read.ok || !strips || strips > SkyGeometry::kMaxVertices / 3 ||
                strips > read.Left() / 91) return Bad(error, "invalid strip count");
            for (uint32_t s = 0; s < strips; ++s) {
                SkyGeometry::Strip strip;
                const auto vertices = read.Word();
                total += vertices;
                if (!read.ok || vertices < 3 || vertices > 38 || vertices > read.Left() / 29 ||
                    total > SkyGeometry::kMaxVertices) return Bad(error, "invalid vertex count");
                for (uint32_t v = 0; v < vertices; ++v) {
                    SkyGeometry::Vertex vertex{};
                    for (float& f : vertex.position) f = read.Float();
                    for (float& f : vertex.stq) f = read.Float();
                    for (uint8_t& c : vertex.colour) c = read.Byte();
                    const auto restart = read.Byte();
                    if (!read.ok || restart > 1) return Bad(error, "invalid restart flag");
                    vertex.restart = restart != 0;
                    strip.vertices.push_back(vertex);
                }
                mesh.strips.push_back(std::move(strip));
            }
            item.model.parts.push_back(std::move(mesh));
        }
        snapshot.models.push_back(std::move(item));
    }
    if (textures > read.Left() / (24 + 32 * 32 * 4)) return Bad(error, "invalid image count");
    for (uint32_t i = 0; i < textures; ++i) {
        SkyAssets::SavedTexture item;
        item.object = read.Word(); item.asset_id = read.Word();
        auto& image = item.image;
        image.width = read.Word(); image.height = read.Word(); image.buffer_width = read.Word();
        image.levels = read.Byte(); image.function = read.Byte();
        const auto alpha = read.Byte(), reserved = read.Byte();
        if (!read.ok || alpha > 1 || reserved || image.width > 128 || image.height > 128 ||
            !read.Raw(image.rgba, size_t(image.width) * image.height * 4))
            return Bad(error, "invalid image extent/flags");
        image.alpha = alpha != 0;
        snapshot.textures.push_back(std::move(item));
    }
    if (!read.ok || read.Left()) return Bad(error, "truncated data or unclassified tail");
    if (!SkyAssets::ValidateSnapshot(snapshot, error)) return false;
    out = std::move(snapshot);
    return true;
}

bool SkyState::Validate(const std::vector<uint8_t>& bytes, std::string* error) {
    SkyAssets::Snapshot snapshot;
    return Decode(bytes, snapshot, error);
}

}  // namespace title
