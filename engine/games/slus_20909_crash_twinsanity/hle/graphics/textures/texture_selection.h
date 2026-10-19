#pragma once

#include <map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_bindings.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_owners.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pairs.h"

namespace title {

// One immutable UI texture policy shared by the load and consumer adapters.
// Calls observe the same stable RAM on the serialized game owner. Readiness
// means full producer/render/restore composition, not just installed codecs.
//
// Selection (the TextureConsumers contract): a UI
// texture ID -- a PSM picture/icon or PSF font page pair (TexturePairs) -- is
// owned when EVERY retail consumer family of that ID is a selected owner
// (Sprite under `sprites`, Font under `text`) and every shader type on it is
// one those producers draw (0x0D; the untextured 0x0E variant). Pictures
// shared with an unownable family (the nine opaque-reference Extras gallery
// pictures), world textures a Sprite borrows (OLEG::SetHudIcon's rigid-model
// material), the particle page the 2D pickup sparkles sample and the Arial
// page stay with the original: their consumers are counted
// as model calls and named in the UnselectedSprites()/DeclinedTextures()
// ledgers (the census rows graphics_sprites_model_by_texture /
// graphics_textures_model_by_texture). Under `textures` those retained
// consumers still draw a CONVERTED image: the model's upload binds the asset to
// the GS slot (TextureUploads), so a converted-but-unselected texture at a
// Sprite/Font consumer is a counted decline, never a fault.
class TextureSelection {
public:
    // `textures`: every texture record converts at load;
    // the title's slot uploads bind the converted image
    // (TextureUploads) so every consumer -- owned producers AND the model's
    // draws through the GS -- samples the same asset. The UI selection
    // (`text`, `sprites`) still decides WHO DRAWS a UI pair.
    struct Services { bool text = false, sprites = false, textures = false; };
    static constexpr uint32_t kTextureTable = 0x3b0240, kTableVtable = 0x2f8dc8;
    // The table's static deletion queue (TR resourcetables.cpp ConstructBase:
    // g_TextureTable.queue = &g_TextureQueue at boot; NTSC .bss 0x3b77a0, a
    // DeletionQueue of 0x2ff items). Released textures wait here for their
    // deletion, outside the table but still allocated.
    static constexpr uint32_t kTextureQueue = 0x3b77a0, kQueueItems = 0x2ff;
    // Detail codes of the counted declines (GraphicsRefusals Sprites/Text rows).
    enum class Decline : uint32_t {
        InvalidExtent = 1,  // consumer/material/shader/font object outside readable RAM
        NoTextureObject,    // selected shader id without a texture object
        MissingAsset,       // selected id this owner never converted (its Read refused)
        LostOwner,          // expected startup material/resource without an owned texture
        EmptyMaterial,      // shader count 0: nothing to own
        EmptyFont,          // page count 0: nothing to own
        ResourceElsewhere,  // material resource points to another material, nothing owned
        Unselected          // converted at load (`textures`) but not this producer's: the original draws it
    };
    TextureSelection(const TextureAssets& assets, TextureBindings::Memory memory, Services ready,
                     GraphicsRefusals* refusals = nullptr);
    bool Texture(uint32_t object, uint32_t asset_id) const;
    // The services this selection was built for (the consumers installed
    // beside it must agree: GraphicsProducers::Install).
    Services Ready() const { return ready_; }
    // The pure policy: the owned UI texture set under the ready services.
    static bool SelectedId(uint32_t asset_id, Services ready) { return Selected(asset_id, ready); }
    // The load policy: every record under `textures`, else the UI set.
    static bool ConvertedId(uint32_t asset_id, Services ready) { return ready.textures || Selected(asset_id, ready); }
    // Directly usable by SpriteService/TextService::Hooks::selected. False
    // proves the observed textures remain outside this selection OR that no
    // texture of this consumer was converted by this owner (a counted DECLINE:
    // the original drew/loads them, so the original must draw this consumer).
    // Throws only when an OWNED texture is involved and the consumer cannot
    // use it (stale/failed generation, identity disagreement, unclassified
    // consumer, mixed page ownership): the original has no upload for that
    // texture, so no arm can draw it -- never a silent fallback.
    // A Sprite whose single type-0E shader is untextured (the retail flat
    // material OLEG gives an icon slot with no picture) needs no texture and
    // is selected under `sprites`: the oracle covers the untextured variant.
    bool Sprite(uint32_t sprite) const;
    bool Font(uint32_t font) const;
    // Pure, bidirectional check of incoming source table and TXAS, including
    // selected entries absent from TXAS and serialized owners absent from RAM.
    // This is validation, not missing-section import or legacy execution.
    static bool PreflightSnapshot(const TextureAssets::Snapshot& snapshot,
                                  TextureBindings::Memory incoming, Services ready,
                                  std::string* error = nullptr);
    // Diagnostic ledgers (game owner only): what this selection left to the
    // original, by texture ID with its TextureConsumers family bits (0xffff
    // = no retail record). Sprites: `texture` 0 = an untextured/unreadable
    // material (the first shader's texture object), counted per call.
    struct Left { uint32_t texture = 0, families = 0; uint64_t count = 0; };
    std::vector<Left> UnselectedSprites() const; // descending count
    std::vector<Left> DeclinedTextures() const;  // descending count

private:
    enum class Consumer { Sprite, Font };
    struct Owner {
        const TexturePairs::Entry* pair = nullptr;
        uint32_t object = 0;
    };
    static bool Selected(uint32_t asset_id, Services ready);
    const uint8_t* Bytes(uint32_t address, size_t size) const;
    // Counted decline: reports through the ledger (if any) and returns false.
    bool Declined(Consumer consumer, Decline code, uint32_t object) const;
    // `why` receives the decline this probe saw (MissingAsset: the original
    // loaded it; Unselected: converted, not this producer's) or stays untouched.
    const TexturePairs::Entry* Probe(uint32_t object, uint32_t declared_id, Decline* why) const;
    Owner Material(uint32_t material, uint32_t resource, Consumer consumer, bool& declined) const;
    bool FlatSprite(uint32_t material) const;
    void LeaveSprite(uint32_t material) const;
    const TextureAssets& assets_;
    TextureBindings::Memory memory_;
    Services ready_;
    GraphicsRefusals* refusals_ = nullptr;
    mutable std::map<uint32_t, uint64_t> unselected_sprites_, declined_textures_;
};

}  // namespace title
