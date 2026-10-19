#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_import.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_codec.h"

namespace title {
namespace {
using Read = TextureImport::Read;
uint32_t Physical(uint32_t address) { return address & 0x1fffffff; }
bool Bad(std::string* error, const std::string& why) {
    if (error) *error = "Texture import: " + why;
    return false;
}
std::string Hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}
template <class T> bool Value(const Read& read, uint32_t address, T& out) {
    return read && address && uint64_t(address) + sizeof(T) <= (uint64_t{1} << 32) &&
           read(address, &out, sizeof(T));
}
bool ValidObject(uint32_t address) {
    const auto segment = address & 0xe0000000, at = Physical(address);
    return !(address & 3) && at && at < 0x02000000 - 0x6c &&
           (segment == 0 || segment == 0x80000000 || segment == 0xa0000000);
}
// DiskLoadedMemory over the image: the handle's node must be the handle's,
// in use and loaded (ready, or read-into and marked loaded).
bool Block(const Read& read, int32_t handle, uint32_t size, std::vector<uint8_t>& out, std::string* why) {
    if (handle < 0 || uint32_t(handle) >= TextureImport::kDiskHandles) { *why = "transfer handle out of range"; return false; }
    if (!size || size > TextureCodec::kMaxRecordBytes) { *why = "transfer size is not a texture record"; return false; }
    uint32_t node = 0;
    std::array<uint32_t, 8> allocation{};
    if (!Value(read, TextureImport::kDiskManager + 0x58 + uint32_t(handle) * 4, node) || !Value(read, node, allocation)) {
        *why = "DiskManager node is unreadable"; return false;
    }
    if (allocation[0] != uint32_t(handle)) { *why = "DiskManager node does not belong to the handle"; return false; }
    const uint32_t bits = allocation[7];
    if (!(bits & 15) || (!(bits & (1 << 14)) && ((bits >> 15) & 3) != 2)) { *why = "transfer block is not loaded"; return false; }
    if (!allocation[1] || allocation[2] < size || uint64_t(allocation[1]) + size > (uint64_t{1} << 32)) {
        *why = "transfer block is smaller than the record"; return false;
    }
    out.resize(size);
    if (!read(allocation[1], out.data(), out.size())) { *why = "transfer block bytes are unreadable"; return false; }
    return true;
}
}  // namespace

bool TextureImport::Derive(const Read& read, TextureSelection::Services ready, Derived& out, std::string* error) {
    using Policy = TextureSelection;
    Derived derived;
    std::array<uint8_t, 0x24> table{};
    if (!Value(read, Policy::kTextureTable, table)) return Bad(error, "image lacks the TextureTable");
    const auto word = [](const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    const auto half = [](const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; };
    const uint32_t entries = word(table.data()), queue = word(table.data() + 0x10);
    const uint32_t count = half(table.data() + 4), capacity = half(table.data() + 6);
    if (word(table.data() + 0x14) != Policy::kTableVtable || half(table.data() + 8) != 0x40 ||
        (queue && queue != Policy::kTextureQueue) || count > capacity || (!entries && capacity) || (entries && !capacity))
        return Bad(error, "image TextureTable has an unclassified owner");
    std::vector<std::pair<uint32_t, uint32_t>> live;   // (object, declared id) rows then queued
    for (uint32_t i = 0; i < count; ++i) {
        std::array<uint32_t, 2> row{};
        if (!Value(read, entries + i * 8, row)) return Bad(error, "image TextureTable rows are unreadable");
        if (row[0]) live.emplace_back(row[0], row[1]);
        ++derived.table_rows;
    }
    if (queue) {
        std::array<uint16_t, 2> ring{};
        if (!Value(read, queue, ring)) return Bad(error, "image deletion queue is unreadable");
        if (ring[0] >= Policy::kQueueItems || ring[1] > Policy::kQueueItems)
            return Bad(error, "image deletion queue is malformed");
        for (uint32_t i = 0; i < ring[1]; ++i) {
            uint32_t item = 0;
            if (!Value(read, queue + 4 + ((ring[0] + i) % Policy::kQueueItems) * 4, item))
                return Bad(error, "image deletion queue items are unreadable");
            if (!item) continue;
            std::array<uint32_t, 2> head{};
            if (!ValidObject(item) || !Value(read, item, head)) return Bad(error, "image queued texture object is unreadable");
            live.emplace_back(item, head[1]);
            ++derived.queued;
        }
    }
    std::unordered_set<uint32_t> seen;
    uint64_t generation = 0;
    for (const auto& [address, declared] : live) {
        if (!ValidObject(address)) return Bad(error, "image texture object " + Hex(address) + " has an invalid extent");
        const uint32_t object = Physical(address);
        if (!seen.insert(object).second) return Bad(error, "image texture object " + Hex(object) + " is listed twice");
        std::array<uint8_t, 0x6c> bytes{};
        if (!Value(read, object, bytes)) return Bad(error, "image texture object " + Hex(object) + " is unreadable");
        const uint32_t id = word(bytes.data() + 4);
        if (id != declared) return Bad(error, "image TextureTable and object " + Hex(object) + " IDs disagree");
        if (!Policy::SelectedId(id, ready)) { ++derived.retained; continue; }
        const int32_t transfer = int32_t(word(bytes.data() + 0x60));
        const uint32_t size = word(bytes.data() + 8);
        if (transfer == -1 || size <= 0x80)
            return Bad(error, "selected texture " + Hex(id) + " has no transfer chain in the image");
        std::vector<uint8_t> storage;
        std::string why;
        if (!Block(read, transfer, size - 0x80, storage, &why))
            return Bad(error, "selected texture " + Hex(id) + ": " + why);
        TextureAssets::SavedAsset asset;
        asset.token = {object, ++generation};
        asset.asset_id = id;
        if (TextureCodec::DecodeParts(bytes.data() + 0x0c, TextureCodec::kHeaderBytes, storage.data(),
                                      storage.size(), asset.image, &why) != TextureCodec::Result::Ok)
            return Bad(error, "selected texture " + Hex(id) + " does not convert: " + why);
        derived.snapshot.assets.push_back(std::move(asset));
        derived.objects.push_back(object);
    }
    derived.snapshot.next_generation = generation + 1;
    if (!TextureAssets::ValidateSnapshot(derived.snapshot, TextureAssets::Limits{}, error)) return false;
    out = std::move(derived);
    if (error) error->clear();
    return true;
}

bool TextureImport::Migrate(uint8_t* ram, size_t size, const std::vector<uint32_t>& objects) {
    if (!ram) return objects.empty();
    for (const auto object : objects) {
        if (object >= size || 0x6c > size - object) return false;
        uint8_t* p = ram + object;
        std::memset(p + 0x60, 0xff, 4);   // no DiskManager transfer (the owned TextureRead's commit)
        std::memset(p + 0x64, 0, 4);      // no upload slot
        std::memset(p + 0x6a, 0, 2);      // first context
    }
    return true;
}

}  // namespace title
