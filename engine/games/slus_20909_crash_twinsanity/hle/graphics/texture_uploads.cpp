#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_uploads.h"

#include <algorithm>
#include <utility>

#include "engine/ee/mem/scratchpad.h"

namespace title {
namespace {
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t Half(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
void Put(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}
uint32_t Physical(uint32_t address) { return address & 0x1fffffff; }
bool InScratchpad(uint32_t address) { return (address & ~Scratchpad::kOffsetMask) == Scratchpad::kBase; }
constexpr uint32_t kBlock = 256;           // BITBLTBUF / TEX0 address unit, bytes
constexpr uint32_t kHeader = 0x60, kTools = 0x20, kEnvelope = 96;
}

TextureUploads::TextureUploads(TextureAssets& assets, Memory memory) : assets_(assets), memory_(memory) {}

uint8_t* TextureUploads::Bytes(uint32_t address, size_t size) const {
    if (InScratchpad(address)) {
        const uint32_t offset = Scratchpad::Offset(address);
        if (!memory_.scratchpad || offset >= memory_.scratchpad_size || offset >= Scratchpad::kSize ||
            size > memory_.scratchpad_size - offset || size > Scratchpad::kSize - offset) return nullptr;
        return memory_.scratchpad + offset;
    }
    const uint32_t segment = address & 0xe0000000, at = Physical(address);
    if (!memory_.data || (segment != 0 && segment != 0x80000000 && segment != 0xa0000000) ||
        !at || at >= 0x02000000 || at >= memory_.size || size > 0x02000000 - at || size > memory_.size - at)
        return nullptr;
    return memory_.data + at;
}
uint32_t TextureUploads::Extent(uint32_t address) {
    return InScratchpad(address) ? address : Physical(address);
}

bool TextureUploads::Describe(const uint8_t* object, const TextureCodec::Image& image, uint64_t key,
                              ps2r::gs::OwnedTextureSlots::Texture& out) {
    using Slots = ps2r::gs::OwnedTextureSlots;
    using Texture = Slots::Texture;
    const uint8_t* h = object + kTextureInObject;
    const uint32_t size = Word(object + 8);
    const uint32_t wp = Half(h + 4), hp = Half(h + 6);
    const unsigned levels = h[8];
    if (wp > 10 || hp > 10 || levels == 0 || levels > Slots::kMaxLevels) return false;
    if (image.levels.size() != levels || image.levels[0].width != (1u << wp) ||
        image.levels[0].height != (1u << hp)) return false;
    const uint32_t psm = h[9];
    if ((psm != 0x13 || image.format != TextureCodec::Format::Indexed8) &&
        (psm != 0x00 || image.format != TextureCodec::Format::Rgba8)) return false;
    if (h[10] != 0) return false;   // the storage image is CT32 (uploadFormat 0)
    if (size < kHeader + kTools + kEnvelope + 16) return false;
    const uint32_t footprint = size - kHeader - kTools - kEnvelope;   // the storage pixels, CT32
    if (footprint % 16 || footprint > TextureCodec::kMaxRecordBytes) return false;
    Texture t;
    t.key = key;
    t.psm = psm;
    t.tw = wp;
    t.th = hp;
    t.levels = uint8_t(levels);
    t.footprint = footprint;
    for (unsigned i = 0; i < levels; ++i) {
        const uint32_t offset = Word(h + 0x10 + i * 4), width = Word(h + 0x2c + i * 4);
        if (offset > 0xffffu || width > 0x3f || (i == 0 && width == 0)) return false;
        t.level_offset[i] = offset * kBlock;
        // TEX0.TBW is the field exactly; a MIPTBP width of 0 reads as one unit (MipSampler).
        t.level_tbw[i] = (i == 0 ? width : std::max(1u, width)) * 64;
        if (t.level_offset[i] >= footprint) return false;
        const auto& level = image.levels[i];
        if (level.width != std::max(1u, (1u << wp) >> i) || level.height != std::max(1u, (1u << hp) >> i) ||
            level.rgba.size() != size_t(level.width) * level.height * 4) return false;
        // The declared (authored) size keys the slot match; the pixels and
        // their extent are the pack's when a pack replaced this image.
        const auto& pixels = image.Pixels()[i];
        t.rgba.push_back({level.width, level.height, pixels.rgba.data(), pixels.width, pixels.height});
    }
    if (psm == 0x13) {
        const uint32_t palette = Word(h + 0x48);
        if (palette > 0x3fffu || palette * kBlock + 1024 > footprint) return false;
        t.clut_offset = palette * kBlock;
    }
    out = std::move(t);
    return true;
}

TextureUploads::Result TextureUploads::Write(uint32_t packet, uint32_t texture, uint32_t slot, uint32_t* packet_end) {
    if (texture < kTextureInObject || (texture & 3)) return Result::InvalidMemory;
    const uint32_t object = texture - kTextureInObject;
    const auto* obj = Bytes(object, 0x6c);
    if (!obj || InScratchpad(object)) return Result::InvalidMemory;
    TextureAssets::View view;
    const auto found = assets_.Find(Physical(object), view);
    if (found == TextureAssets::Result::MissingAsset) return Result::NotOwned;
    if (found != TextureAssets::Result::Ok || !view.image) return Result::ConflictingOwner;
    // The owned object carries no DiskManager transfer: this owner's Read wrote it.
    if (Word(obj + 0x60) != 0xffffffff || Word(obj + 4) != view.asset_id) return Result::ConflictingOwner;
    const auto* s = Bytes(slot, 0x10);
    uint8_t* p = Bytes(packet, 16);
    if (!s || !p || (slot & 3) || (packet & 15)) return Result::InvalidMemory;
    const uint32_t base = Word(s) * kBlock;   // TextureSlot::gsAddress, 256-byte units
    Upload upload;
    upload.base = base;
    upload.tag = Extent(packet);
    upload.object = Physical(object);
    upload.frame = frame_;
    if (!Describe(obj, *view.image, view.token.generation, upload.texture)) return Result::InvalidHeader;
    upload.texture.pin = view.image;
    // Everything is validated: the one guest write, then the record.
    Put(p, kEmptyCntTag);
    Put(p + 4, 0);
    Put(p + 8, 0);
    Put(p + 12, 0);
    const uint32_t key = upload.tag;
    auto at = pending_.find(key);
    if (at != pending_.end()) { ++counts_.overwritten; at->second = std::move(upload); }
    else pending_.emplace(key, std::move(upload));
    counts_.pending = pending_.size();
    *packet_end = packet + 16;
    return Result::Ok;
}

bool TextureUploads::TakeTag(uint32_t address, bool scratchpad, Upload& out) {
    if (pending_.empty()) return false;
    const uint32_t key = scratchpad ? (Scratchpad::kBase | (address & Scratchpad::kOffsetMask)) : Physical(address);
    auto at = pending_.find(key);
    if (at == pending_.end()) return false;
    out = std::move(at->second);
    pending_.erase(at);
    ++counts_.taken;
    counts_.pending = pending_.size();
    return true;
}

void TextureUploads::SetFrame(uint64_t frame) {
    if (frame <= frame_) return;
    frame_ = frame;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.frame + kPendingFrames <= frame_) { ++counts_.discarded; it = pending_.erase(it); }
        else ++it;
    }
    counts_.pending = pending_.size();
}

bool TextureUploads::Install(const Capture& capture, const Installer& install) {
    if (!capture || !install) return false;
    const auto original = capture(kWriteUpload);
    if (!original) return false;
    return install(kWriteUpload, [this, original](CpuContext& c, uint32_t entry) {
        ++counts_.calls;
        if (entry && entry != kWriteUpload) { original(c, entry); return; }
        const uint32_t packet = c.cpuRegs.GPR.r[5].UL[0], texture = c.cpuRegs.GPR.r[6].UL[0],
                       slot = c.cpuRegs.GPR.r[7].UL[0];
        uint32_t end = 0;
        const Result result = Write(packet, texture, slot, &end);
        if (result == Result::Ok) {
            ++counts_.owned;
            c.cpuRegs.GPR.r[2].SD[0] = int32_t(end);
            return;
        }
        if (result == Result::NotOwned) ++counts_.declined; else ++counts_.refused;
        original(c, entry);
    });
}

}  // namespace title
