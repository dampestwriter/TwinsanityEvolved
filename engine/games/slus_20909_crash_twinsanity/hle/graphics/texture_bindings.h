#pragma once

#include <functional>

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_refusals.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_assets.h"

namespace title {

// Source-level TextureRead/TextureDestroy adapters. Calls and guest memory
// mutation run on the serialized game owner, never on the renderer.
class TextureBindings {
public:
    static constexpr uint32_t kRead = 0x1bc920, kDestroy = 0x1c0988;
    static constexpr uint32_t kMemoryStream = 0x306b50;
    enum class Result {
        Ok, NotSelected, NotOwned, InvalidMemory, UnsupportedStream, InvalidStream,
        ConflictingOwner, InvalidAsset, UnsupportedAsset, ResourceFailure,
        MissingDeallocator, UnsupportedMaterial, InheritedMaterial, UnsupportedLod
    };
    // Ownership word: an object this owner converted carries NO DiskManager
    // transfer (+0x60 == 0xffffffff). Its slot word (+0x64) is the title's own
    // texture-slot allocator state and may be set once a model consumer uploads
    // the texture through TextureUploads (the owned slot bind).
    // Stable, writable contiguous physical RAM for the duration of every call.
    // All output extents are validated before Load, then committed without
    // callbacks/yields. No borrowed pointer survives a call. `scratchpad` is
    // the 16 KB window at 0x70000000: the title's main thread runs on the
    // scratchpad (TR src/platform/ps2/entry.cpp:83; stack.h:5-8), so the
    // Stream a TextureRead receives is a stack object there (the startup icons
    // and fonts: gamecontroller.cpp:448, font.cpp:131). Without the window
    // every scratchpad extent is InvalidMemory. Texture objects themselves are
    // heap (RAM) only: their physical address keys the persisted asset.
    struct Memory {
        uint8_t* data = nullptr; size_t size = 0;
        uint8_t* scratchpad = nullptr; size_t scratchpad_size = 0;
    };
    // Pure eligibility check: every consumer of a selected asset must use this
    // owner. Must not mutate RAM, yield, or change selection after publication.
    using Select = std::function<bool(uint32_t object, uint32_t asset_id)>;
    using Body = std::function<void(CpuContext&, uint32_t)>;
    using Capture = std::function<Body(uint32_t)>;
    using Installer = std::function<bool(uint32_t, Body)>;
    using FreeObject = std::function<void(uint32_t)>;
    using Deallocate = std::function<void(CpuContext&, uint32_t)>;
    // After this owner RELEASED a converted asset (the source destroyed its
    // object, or re-read / transferred into it): the token that was dropped.
    // The slot-upload owner retires the asset's binds and surfaces on it.
    using Released = std::function<void(TextureAssets::Token)>;
    // declined = unselected/unowned calls delegated by policy (the model arm);
    // refused = selected calls returned to the original BEFORE any commit
    // (attributed through the refusal sink); failed = faults after commit.
    struct Counters {
        uint64_t calls = 0, loaded = 0, destroyed = 0, declined = 0, failed = 0, refused = 0;
    };
    enum class Source { None, Asset, SceneCopy };
    enum class Filter { Nearest, Linear, LinearMipNearest };
    enum class Wrap { Repeat, Clamp };
    struct Material {
        Source source = Source::None;
        TextureAssets::View asset;
        Filter magnify = Filter::Nearest, minify = Filter::Nearest;
        Wrap wrap = Wrap::Repeat;
        bool stq = false, alpha = false;
        uint8_t function = 0, max_level = 0, lod_shift = 0;
        int16_t lod_bias = 0;  // signed sixteenths
        uint32_t type = 0;
    };

    TextureBindings(TextureAssets& assets, Memory memory, Select selected);
    void SetReleased(Released released);
    Result Read(uint32_t object, uint32_t stream, TextureAssets::View& out, std::string* error = nullptr);
    Result Destroy(uint32_t object, uint32_t flags, const FreeObject& free_object);
    Result CaptureMaterial(uint32_t shader, Material& out) const;
    // The converted image of an object this owner PUBLISHED: the ownership word
    // (+0x60 == 0xffffffff) and the asset id agree with the owned record. Null
    // for an object the original loaded (its DiskManager handle is at +0x60)
    // or one this owner has released. Pure; another load-boundary owner (the
    // sky's SkyAssets) reads the texture from here instead of the transfer block.
    std::shared_ptr<const TextureCodec::Image> Converted(uint32_t object) const;
    // The published asset's View (handle, lease, image, token) under the same
    // agreement as Converted: the mesh bucket owner's texture lookup
    // (`TextureAssets::Find(object)` exactly as
    // MeshMaterial::Lookup). False for an object this owner never published.
    bool View(uint32_t object, TextureAssets::View& out) const;
    // The asset owner itself (consumer lookups by lease: TextureAssets::Find(lease)).
    const TextureAssets& Assets() const { return assets_; }
    // Pure preflight against INCOMING RAM, before any live restore mutation.
    // Proves each serialized owner's source object/header and selection agree.
    // Does not enumerate every source table or authorize missing TXAS/legacy
    // import; root must also validate section presence and complete ownership.
    static bool ValidateSnapshotMemory(const TextureAssets::Snapshot& snapshot, Memory incoming,
                                       const Select& selected, std::string* error = nullptr);
    // Capture originals before either install. Unselected reads / unowned
    // destroys delegate by policy (declined). A selected call whose object,
    // stream or asset this owner cannot take (invalid memory, unsupported or
    // invalid stream, conflicting transfer/slot words, unconvertible asset,
    // resource limit) REFUSES: it is counted, reported through `refuse` with
    // Result as the detail code, and the original body runs -- nothing was
    // committed. A conflicting object this owner already converted first drops
    // that claim (Release) so every later consumer sees the original's texture.
    // An interior continuation refuses the same way (detail 0x1000). Faults
    // (a throw after the guest commit) propagate; never replay failed work. False may leave an installed prefix:
    // root must fail startup. Deallocate must perform the real title heap free
    // (0x181590), synchronously without yielding/reentering this owner.
    bool Install(const Capture& capture, const Installer& install, Deallocate deallocate,
                 GraphicsRefusals::Sink refuse = {});
    // Reason the installed wrapper reports for a refusing Result.
    static GraphicsRefusals::Reason RefusalReason(Result result);
    Counters Counts() const { return counts_; }

private:
    // RAM in any KUSEG/KSEG0/KSEG1 alias, or the scratchpad window.
    uint8_t* Bytes(uint32_t address, size_t size) const;
    // RAM only: objects this owner keys and persists by physical address.
    uint8_t* RamBytes(uint32_t address, size_t size) const;
    // Disjoint extent key: RAM physical address, or 0x70000000 | offset.
    static uint32_t Extent(uint32_t address);
    Result StreamRecord(uint32_t object, uint32_t stream, uint8_t*& cursor,
                        uint8_t*& record, size_t& size) const;
    TextureAssets& assets_;
    Memory memory_;
    Select selected_;
    Released released_;
    Counters counts_;
};

}  // namespace title
