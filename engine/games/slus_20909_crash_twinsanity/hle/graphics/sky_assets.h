#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_texture.h"

namespace title {

// Load-boundary conversion cache. Keys are live source OBJECTS, never texture
// addresses or asset IDs (the archive reuses IDs with different content).
// Each replacement gets a fresh shared allocation. Recorded work retains it
// after destruction, compaction, reload, or reuse of the source address.
class SkyAssets {
public:
    static constexpr uint32_t kModelRead = 0x001bcb58, kModelDestroy = 0x001c0b08;
    static constexpr uint32_t kTextureRead = 0x001bc920, kTextureDestroy = 0x001c0988;
    static constexpr uint32_t kDiskManager = 0x0031b138;
    struct Model { std::vector<SkyGeometry::Mesh> parts; };
    using ModelLease = std::shared_ptr<const Model>;
    using TextureLease = std::shared_ptr<const SkyTexture::Image>;
    struct SavedModel { uint32_t object = 0, asset_id = 0; Model model; };
    struct SavedTexture { uint32_t object = 0, asset_id = 0; SkyTexture::Image image; };
    struct Snapshot {
        std::vector<SavedModel> models;
        std::vector<SavedTexture> textures;
    };
    enum class Result { Ok, InvalidMemory, UnsupportedAsset, LimitExceeded, NotConverted };
    // The converted image of a texture object another load-boundary owner
    // published (TextureBindings::Converted under hle.graphics_textures), or
    // null. Consulted when the object's transfer word (+0x60) is the ownership
    // word 0xffffffff: that object has NO DiskManager block to read.
    using Converted = std::function<std::shared_ptr<const TextureCodec::Image>(uint32_t object)>;
    // Call after the complete source reader returns. Failure erases the old
    // version, preventing a stale replacement from surviving a failed reload.
    Result ModelLoaded(const MaterialState::Read& read, uint32_t object);
    // An object the original loaded is read from its DiskManager block (the
    // model arm); one the texture owner converted is taken from `converted`
    // (NotConverted when it cannot be: the owner released it or no lookup).
    Result TextureLoaded(const MaterialState::Read& read, uint32_t object, const Converted& converted = {});
    void ForgetModel(uint32_t object);
    void ForgetTexture(uint32_t object);
    ModelLease FindModel(uint32_t object) const;
    TextureLease FindTexture(uint32_t object) const;
    // Complete converted owner, at a fence with no outstanding asset leases.
    // `unlinked_recording_open`: the ordered output's capture-time settle
    // (GraphicsOutput::SettleOwnedForCapture) established that every pending
    // owned item is the frame's recorded, not yet Linked sky pass; the leases
    // its recorded draws hold are the open recording's -- dropped from the
    // image with it (GXPD) -- and do not refuse the capture (the TextureAssets
    // rule). At a closed boundary (false) the guard stays exact.
    // Restore only into a fresh owner; no source reader, packet, or backend use.
    bool Capture(Snapshot&, std::string* error = nullptr, bool unlinked_recording_open = false) const;
    bool Restore(const Snapshot&, std::string* error = nullptr);
    static bool ValidateSnapshot(const Snapshot&, std::string* error = nullptr);
    // Software object IDs, submodel counts and texture descriptors in incoming
    // RAM must agree. Mutable references, disk relocation and texture slots are
    // deliberately not identities of these immutable converted allocations.
    static bool Preflight(const Snapshot&, const MaterialState::Read&, std::string* error = nullptr);
    void Reset();

private:
    struct ModelEntry { uint32_t asset_id; ModelLease lease; };
    struct TextureEntry { uint32_t asset_id; TextureLease lease; };
    std::map<uint32_t, ModelEntry> models_;
    std::map<uint32_t, TextureEntry> textures_;
};

}  // namespace title
