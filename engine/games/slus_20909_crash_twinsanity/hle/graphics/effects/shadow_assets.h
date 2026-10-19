#pragma once

#include <map>
#include <memory>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// Serialized game owner. ModelRead completion converts source asset bytes;
// render submissions retain immutable versions and never consult the disk table.
class ShadowAssets {
public:
    static constexpr uint32_t kRead = 0x001bcb58, kDestroy = 0x001c0b08;
    static constexpr uint32_t kDiskManager = 0x0031b138;
    static constexpr uint32_t kMeshes = 0x003d6b00;
    // The type-0B shader vtable the NTSC shader factory stores (ReadMaterialShaders
    // 1bb710 at 1bb948: shader+0x6c = 0x2fa8c8, then SetUp 1d9338 writes type 0B).
    // Slot 1 destroy 1d9298, slot 8 NeedsEye 1d9330 (answers 1: every default
    // shadow mesh carries the eye instance size), slot 13 SetUp 1d9338.
    static constexpr uint32_t kShaderVtable = 0x002fa8c8;
    static constexpr size_t kMaxCurrent = 16, kMaxVersions = 64;
    struct Token { uint32_t object = 0; uint64_t generation = 0; };
    struct Asset {
        Token token;
        uint32_t id = 0, vertices = 0;
        ShadowGeometry::Mesh mesh;
        std::vector<uint8_t> bytes;
    };
    using Lease = std::shared_ptr<const Asset>;
    enum class Result {
        Ok, NotSelected, Missing, InvalidMemory, InvalidAsset, LimitExceeded,
        RetainedConsumer, InvalidSnapshot, OwnerNotEmpty
    };
    struct Saved {
        Token token;
        uint32_t id = 0, vertices = 0;
        std::vector<uint8_t> bytes;
    };
    struct Snapshot {
        uint32_t version = 1;
        uint64_t next_generation = 1; // zero represents exhaustion, never reset
        std::vector<Saved> assets; // canonical ascending generation
    };
    struct Counters { uint64_t loaded = 0, released = 0, rejected = 0; };

    // Eight default-package IDs from the complete static inventory, plus
    // LoadShadowMeshes(true)'s explicitly declared development-file IDs.
    static bool IsSourceModel(uint32_t id);
    // Call only after a complete ModelRead. Invalidate an earlier object BEFORE
    // reading its new contents: a failed reload must never serve the old model.
    Result ModelLoaded(const MaterialState::Read&, uint32_t object);
    Lease Find(uint32_t object) const;
    void Forget(uint32_t object);
    Result ResetAfterQuiescence();
    Counters Counts() const { return counts_; }
    size_t Current() const { return current_.size(); }

    // Quiescent capture refuses retained consumers, including retired versions.
    // Persistence keeps owned bytes and identity, never pointers into a model.
    Result Capture(Snapshot&) const;
    static bool Validate(const Snapshot&);
    // Pure incoming-RAM identity proof. It does not execute a model or require
    // the old disk allocation to remain resident after load-time conversion.
    static bool ValidateSource(const Snapshot&, const MaterialState::Read&);
    // DrawShadowEntry's complete eight-slot source consumer inventory.
    // Every installed shape must resolve to the saved geometry generation;
    // foreign source instance blocks/material writers must already be closed.
    static bool ValidateConsumers(const Snapshot&, const MaterialState::Read&);
    // Fresh owner only. Failure leaves this owner unchanged; absent payload is
    // not an empty Snapshot and must be rejected by the root selection preflight.
    Result Restore(const Snapshot&);
private:
    std::map<uint32_t, Lease> current_;
    std::vector<std::weak_ptr<const Asset>> versions_;
    uint64_t next_generation_ = 1;
    Counters counts_;
    void Collect();
    bool HasConsumers() const;
};

}  // namespace title
