#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pairs.h"

#include <algorithm>

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pairs_catalogue.h"

namespace title {
namespace {
using Catalogue = TexturePairsCatalogue;
}  // namespace

const char* TexturePairs::ArchiveHeaderSha256() { return Catalogue::HeaderSha256(); }
const char* TexturePairs::ArchiveDataSha256() { return Catalogue::DataSha256(); }
size_t TexturePairs::Size() {
    size_t size = 0;
    for (size_t chunk = 0; chunk < Catalogue::kChunks; ++chunk) size += Catalogue::Chunk(chunk).size;
    return size;
}
const TexturePairs::Entry* TexturePairs::At(size_t index) {
    for (size_t chunk = 0; chunk < Catalogue::kChunks; ++chunk) {
        const auto rows = Catalogue::Chunk(chunk);
        if (index < rows.size) return rows.data + index;
        index -= rows.size;
    }
    return nullptr;
}
const TexturePairs::Entry* TexturePairs::Find(uint32_t texture) {
    for (size_t chunk = 0; chunk < Catalogue::kChunks; ++chunk) {
        const auto rows = Catalogue::Chunk(chunk);
        const auto* found = std::lower_bound(rows.data, rows.data + rows.size, texture,
            [](const Entry& entry, uint32_t id) { return entry.texture < id; });
        if (found != rows.data + rows.size && found->texture == texture) return found;
    }
    return nullptr;
}
const TexturePairs::Entry* TexturePairs::FindMaterial(uint32_t material) {
    for (size_t chunk = 0; chunk < Catalogue::kChunks; ++chunk) {
        const auto rows = Catalogue::Chunk(chunk);
        const auto* end = rows.by_material + rows.size;
        const auto* found = std::lower_bound(rows.by_material, end, material,
            [&rows](uint16_t index, uint32_t id) { return rows.data[index].material < id; });
        if (found != end && rows.data[*found].material == material) return rows.data + *found;
    }
    return nullptr;
}

}  // namespace title
