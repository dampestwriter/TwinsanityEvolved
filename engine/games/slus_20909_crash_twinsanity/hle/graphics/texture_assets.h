#pragma once

#include <memory>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_codec.h"

namespace title {
class TexturePack;
class TextureDump;

// Converted asset ownership, independent of the title's evictable upload slots.
// Producer Load/Find/Release/Record run on the serialized game owner. Upload
// and CollectAfterQuiescence run on the backend owner; neither reads guest RAM.
class TextureAssets {
public:
    enum class Result { Ok, InvalidObject, InvalidAsset, UnsupportedAsset, LimitExceeded,
                        MissingAsset, StaleGeneration, QueueRejected, NotOwned,
                        InvalidUpload, BackendFailed, PendingWork, RetainedConsumer,
                        InvalidSnapshot, OwnerNotEmpty };
    struct Token { uint32_t object = 0; uint64_t generation = 0; };
    struct View {
        Token token;
        uint32_t asset_id = 0;
        Graphics::Handle handle;
        Graphics::Lease lease;
        std::shared_ptr<const TextureCodec::Image> image;
    };
    struct Limits { size_t versions = 4096; uint64_t bytes = 128 * 1024 * 1024; };
    struct Counters {
        uint64_t loaded = 0, released = 0, uploaded = 0, failed = 0;
        uint64_t retained_bytes = 0;
        size_t current = 0, versions = 0;
    };
    struct SavedAsset {
        Token token;
        uint32_t asset_id = 0;
        TextureCodec::Image image;
    };
    struct Snapshot {
        uint32_t version = 1;
        uint64_t next_generation = 1; // zero preserves an exhausted generation space
        std::vector<SavedAsset> assets; // canonical ascending generation order
    };
    TextureAssets(Graphics& queue, GraphicsResources& resources, ps2r::render::Backend& backend);
    TextureAssets(Graphics& queue, GraphicsResources& resources, ps2r::render::Backend& backend, Limits limits);
    // Reset on the producer while the queue is mutable before destroying this
    // owner; drain consumers before destruction. Backend outlives both owners.
    ~TextureAssets();
    TextureAssets(const TextureAssets&) = delete;
    TextureAssets& operator=(const TextureAssets&) = delete;
    // The texture pack consulted at every Load/Restore (by TextureContentHash;
    // a hit replaces the GPU pixels, never the authored identity) and the
    // --dump-textures writer told of every Load. Producer side, before the
    // first Load; both outlive this owner. Null = none.
    void SetPack(TexturePack* pack);
    void SetDump(TextureDump* dump);
    struct PackCounters { uint64_t replaced = 0; };
    PackCounters PackCounts() const;

    // Converts ALL bytes needed for every authored level now, then registers a
    // fresh allocation version. No backend call. Reuse of an object/id creates
    // a new generation while queued draws keep the previous version alive.
    // Failures preserve the current asset and output View.
    Result Load(uint32_t object, uint32_t asset_id, const uint8_t* record, size_t size,
                View& out, std::string* error = nullptr);
    Result Find(uint32_t object, View& out) const;
    Result Find(Token token, View& out) const;
    // Consumer lookup for a retained draw allocation, including released or
    // superseded game objects. Does not redirect to the object's current image.
    Result Find(const Graphics::Lease& allocation, View& out) const;
    // Consumer lookup by the title's texture item id and the exact allocation
    // generation a producer captured (effects/materials). The returned View
    // pins that immutable converted image and allocation for as long as it is
    // held, across the ordered frame; a superseded or released generation is
    // StaleGeneration (never the asset's current image), an id this owner has
    // never converted is MissingAsset. Producer side; no backend call.
    Result Lease(uint32_t asset_id, uint64_t generation, View& out) const;
    Result Release(Token token);
    Result Reset();
    Result Record(const Graphics::Anchor& before, Token token);

    // Suitable for the root's ordered Graphics::Hooks::upload. NotOwned means
    // the callback belongs to another resource owner; all other failures stop
    // submission. The SAME allocation receives level 0, all authored mips,
    // then FinishMipChain. Parent supplies its existing enhancement policy.
    Result Upload(const Graphics::Upload& upload, bool synthesize_tail);
    Result CollectAfterQuiescence();
    Counters Counts() const;

    // Quiescent boundary only, on the serialized producer after its consumer
    // fence. No mutation. Recording/faulted frames and external texture leases
    // refuse: the shared queue has no ordered checkpoint/rebinding API yet.
    // Expired garbage is omitted; a live retired version is NEVER discarded.
    // `unlinked_recording_open`: the ordered output's capture-time settle
    // (GraphicsOutput::SettleOwnedForCapture) established that every pending
    // owned item is the frame's recorded, not yet Linked work; the leases
    // those captured draws hold are then the open recording's -- dropped from
    // the image with it (GXPD) -- and do not refuse the capture. At a closed
    // boundary (false) the guard stays exact.
    Result Capture(Snapshot& out, std::string* error = nullptr, bool unlinked_recording_open = false) const;
    static bool ValidateSnapshot(const Snapshot& snapshot, Limits limits, std::string* error = nullptr);
    // Pure value/local-owner preflight. Restore requires a fresh TextureAssets
    // owner and an idle shared queue; it never resets an existing owner.
    Result PreflightRestore(const Snapshot& snapshot, std::string* error = nullptr) const;
    // Register fresh allocations and preserve object/id/generation. No backend
    // calls. All restored tokens require Record before their first draw; output
    // lists them without inventing a frame, upload order or completion credit.
    // Failure preserves this owner and output. Failed staged registrations are
    // released; GraphicsResources collects their metadata on its fenced owner.
    Result Restore(const Snapshot& snapshot, std::vector<Token>& uploads, std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
