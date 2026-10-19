#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>

#include "engine/ee/cpu/cpu_state.h"
#include "engine/gs/hle/texsrc/owned_texture_slots.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_assets.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_bindings.h"

namespace title {

// The source WriteTextureUpload adapter (TR renderer/textures.cpp
// WriteTextureUpload = SetTextureDma_, NTSC 0x1c0880): the title's per-frame
// upload of a texture into one of its GS slots -- a CNT tag of VIF1's DIRECT of
// two quadwords (the A+D BITBLTBUF naming the slot) and a CALL of the record's
// transfer chain (the storage image, PATH2 IMAGE data). For a texture this
// owner converted at load (TextureAssets holds its image; the object carries
// no DiskManager transfer, +0x60 == 0xffffffff) the adapter writes ONE empty
// CNT tag in the packet's place (16 bytes, no VIF data: the chain walks over
// it) and records, keyed by that tag's address, the slot BIND the walk delivers
// in the tag's stream position: the slot base, the bytes the transfer would
// have covered, the TEX0 tuple the title reads the slot with (derived from the
// object's header exactly as TR textures.cpp WriteTexturePairs derives it) and
// the converted levels. The texture resolver serves that bind
// (OwnedTextureSlots); no PATH2 byte, VRAM write or decode happens for it.
// Objects this owner never converted run the original (declined); a converted
// object whose header or slot the adapter cannot read refuses (the original
// runs, nothing was written). Calls and guest writes run on the serialized
// game owner; TakeTag runs on the DMAC walk owner (the same thread in this
// runtime's composition -- the chain is walked from the title's Link).
class TextureUploads {
public:
    static constexpr uint32_t kWriteUpload = 0x1c0880;
    static constexpr uint32_t kTextureInObject = 0x0c;   // GameTexture: vtable, id, size, Texture
    static constexpr uint32_t kEmptyCntTag = 0x10000000;  // DMAtag CNT, QWC 0, VIF NOP NOP
    // A chain is walked up to two Links after its packets were written (the
    // title builds frame N while N-1 presents; the VIF1 kick is deferred to the
    // SyncV): a pending bind lives this many source frames before it is dropped.
    static constexpr uint64_t kPendingFrames = 4;
    using Body = TextureBindings::Body;
    using Capture = TextureBindings::Capture;
    using Installer = TextureBindings::Installer;
    using Memory = TextureBindings::Memory;
    enum class Result { Ok, NotOwned, InvalidMemory, ConflictingOwner, InvalidHeader };
    struct Upload {
        uint32_t base = 0;              // slot base, bytes
        uint32_t tag = 0;               // the empty tag's extent key (physical RAM / scratchpad)
        uint32_t object = 0;            // the physical texture object
        uint64_t frame = 0;             // FrameTick count at the call
        ps2r::gs::OwnedTextureSlots::Texture texture;
    };
    struct Counters {
        uint64_t calls = 0, owned = 0, declined = 0, refused = 0;
        uint64_t taken = 0;             // binds the walk delivered
        uint64_t discarded = 0;         // pending binds whose tag was never walked (chain reset)
        uint64_t overwritten = 0;       // a packet address re-recorded while its bind was pending
        uint64_t pending = 0;           // live
    };

    TextureUploads(TextureAssets& assets, Memory memory);
    // Capture the original before install. False = nothing installed.
    bool Install(const Capture& capture, const Installer& install);
    // The adapter's body over the title's arguments (a1 packet, a2 Texture*, a3
    // slot); Ok wrote the empty tag and recorded the pending bind, `packet_end`
    // = the packet cursor after it. Every other result wrote nothing.
    Result Write(uint32_t packet, uint32_t texture, uint32_t slot, uint32_t* packet_end);
    // The VIF1 walk passed the tag at `address` (physical RAM, or a scratchpad
    // offset when `scratchpad`): true = an owned upload stood there; `out` is it.
    bool TakeTag(uint32_t address, bool scratchpad, Upload& out);
    // The source frame clock (the Link ordinal, GraphicsOutput::SourceFrame):
    // binds recorded kPendingFrames before `frame` and never walked were dropped
    // with their chain (TR context.cpp ResetBuckets after WaitIdle).
    void SetFrame(uint64_t frame);
    Counters Counts() const { return counts_; }
    size_t Pending() const { return pending_.size(); }

    // The bind description from the texture object's header (TR renderer.h
    // Texture at +0x0c: widthPower/heightPower, levels, pixelFormat, the per-level
    // block offsets and 64-pixel widths, the palette block) and the converted
    // image; `size` is the record size word (+8). False = the header does not
    // describe `image` (levels, dimensions, format) or names an extent outside
    // the transfer's footprint.
    static bool Describe(const uint8_t* object, const TextureCodec::Image& image, uint64_t key,
                         ps2r::gs::OwnedTextureSlots::Texture& out);

private:
    uint8_t* Bytes(uint32_t address, size_t size) const;
    static uint32_t Extent(uint32_t address);
    TextureAssets& assets_;
    Memory memory_;
    Counters counts_;
    uint64_t frame_ = 0;
    std::map<uint32_t, Upload> pending_;   // tag extent -> the bind it delivers
};

}  // namespace title
