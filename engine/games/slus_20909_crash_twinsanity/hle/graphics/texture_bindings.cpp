#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_bindings.h"
#include "engine/ee/timing/tick_profile.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "engine/ee/mem/scratchpad.h"

namespace title {
namespace {
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
int SignedHalf(const uint8_t* p) {
    const unsigned value = unsigned(p[0]) | unsigned(p[1]) << 8;
    return value & 0x8000 ? int(value) - 0x10000 : int(value);
}
void Put(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}
uint32_t Physical(uint32_t address) { return address & 0x1fffffff; }
// The 16 KB scratchpad exactly (0x70000000..0x70003fff: TR entry.cpp:61-62,83);
// no mirror above it is mapped on the EE.
bool InScratchpad(uint32_t address) { return (address & ~Scratchpad::kOffsetMask) == Scratchpad::kBase; }
bool Overlaps(uint32_t a, size_t an, uint32_t b, size_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}
// pal_ntsc_map's ShaderTypeXXSettings rows, verified against NTSC vtable
// words in ram_data_map_v50. Never call these bodies; prove the indirect owner.
constexpr uint32_t kSettings[] = {
    0x1d0590, 0x1d45e0, 0x1cf7f0, 0x1cee58, 0x1d7be0, 0x1dc7a0, 0x1dc2a8, 0x1dc478,
    0x1dc590, 0x1dc6a8, 0x1d81e0, 0x1dc0e0, 0x1ce0d0, 0x1ce778, 0x1d3f38, 0x1d6828,
    0x1d5628, 0x1d7400, 0x1d0960, 0x1cdd10, 0x1cd920, 0x1cfec0, 0x1d2ac8, 0x1d18c8,
    0x1d5f90, 0x1d23f8, 0x1d0d20, 0x1d3868, 0x1da908, 0, 0x1d3198, 0x1cf110, 0x1d6e00,
};
}
TextureBindings::TextureBindings(TextureAssets& assets, Memory memory, Select selected)
    : assets_(assets), memory_(memory), selected_(std::move(selected)) {}
void TextureBindings::SetReleased(Released released) { released_ = std::move(released); }

uint8_t* TextureBindings::RamBytes(uint32_t address, size_t size) const {
    const uint32_t segment = address & 0xe0000000, at = Physical(address);
    if (!memory_.data || (segment != 0 && segment != 0x80000000 && segment != 0xa0000000) ||
        !at || at >= 0x02000000 || at >= memory_.size ||
        size > 0x02000000 - at || size > memory_.size - at) return nullptr;
    return memory_.data + at;
}
uint8_t* TextureBindings::Bytes(uint32_t address, size_t size) const {
    if (!InScratchpad(address)) return RamBytes(address, size);
    const uint32_t offset = Scratchpad::Offset(address);
    if (!memory_.scratchpad || offset >= memory_.scratchpad_size || offset >= Scratchpad::kSize ||
        size > memory_.scratchpad_size - offset || size > Scratchpad::kSize - offset) return nullptr;
    return memory_.scratchpad + offset;
}
uint32_t TextureBindings::Extent(uint32_t address) {
    return InScratchpad(address) ? address : Physical(address);
}
TextureBindings::Result TextureBindings::StreamRecord(uint32_t object, uint32_t stream,
                                                     uint8_t*& cursor, uint8_t*& record, size_t& size) const {
    auto* s = Bytes(stream, 0x14);
    if (!s || (stream & 3)) return Result::InvalidMemory;
    const auto* v = RamBytes(Word(s), 0xb0);
    // Both the vtable's identity and the actual indirect Read/ReadU32 targets
    // are verified. Patched subclasses/this-adjustments need their own owner.
    if (!v || Word(s) != kMemoryStream || SignedHalf(v + 0x10) || SignedHalf(v + 0xa8) ||
        Word(v + 0x14) != 0x2b5dd8 || Word(v + 0xac) != 0x2b5850) return Result::UnsupportedStream;
    const uint32_t begin = Word(s + 8), position = Word(s + 12), length = Word(s + 16);
    if ((begin & 0xe0000000) != (position & 0xe0000000) || position < begin ||
        uint64_t(position) - begin > length || !Bytes(begin, length)) return Result::InvalidStream;
    const size_t left = length - (position - begin);
    if (left < 4 || !(record = Bytes(position, 4))) return Result::InvalidStream;
    size = size_t(Word(record)) + 4;
    if (size < 132 || size > TextureCodec::kMaxRecordBytes || size > left ||
        !Bytes(position, size)) return Result::InvalidStream;
    const uint32_t obj = Extent(object), stm = Extent(stream), data = Extent(position);
    if (Overlaps(obj, 0x6c, stm, 0x14) || Overlaps(obj, 0x6c, data, size) ||
        Overlaps(stm, 0x14, data, size) || Overlaps(obj, 0x6c, kMemoryStream, 0xb0) ||
        Overlaps(stm, 0x14, kMemoryStream, 0xb0)) return Result::InvalidStream;
    cursor = s + 12;
    return Result::Ok;
}
TextureBindings::Result TextureBindings::Read(uint32_t object, uint32_t stream,
                                             TextureAssets::View& out, std::string* error) {
    ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Texture);
    auto* obj = RamBytes(object, 0x6c);
    if (!obj || (object & 3)) return Result::InvalidMemory;
    TextureAssets::View previous;
    const auto ownership = assets_.Find(Physical(object), previous);
    if (ownership != TextureAssets::Result::Ok && ownership != TextureAssets::Result::MissingAsset)
        return Result::ResourceFailure;
    if (ownership == TextureAssets::Result::MissingAsset &&
        (!selected_ || !selected_(Physical(object), Word(obj + 4)))) return Result::NotSelected;
    if (Word(obj + 0x60) != 0xffffffff ||
        (ownership == TextureAssets::Result::Ok && previous.asset_id != Word(obj + 4))) {
        // The source is re-reading or transferring into an object this owner
        // converted. Drop the claim now: the original takes the object whole and
        // every consumer's Find reports MissingAsset instead of a stale image.
        if (ownership == TextureAssets::Result::Ok) {
            if (assets_.Release(previous.token) != TextureAssets::Result::Ok) return Result::ResourceFailure;
            if (released_) released_(previous.token);
        }
        return Result::ConflictingOwner;
    }
    uint8_t* cursor = nullptr;
    uint8_t* record = nullptr;
    size_t size = 0;
    const auto stream_result = StreamRecord(object, stream, cursor, record, size);
    if (stream_result != Result::Ok) return stream_result;
    std::array<uint8_t, 100> header;
    std::memcpy(header.data(), record, header.size());
    Put(header.data() + 0x58, 0xffffffff); // object+0x60: no DiskManager transfer -- the ownership word
    Put(header.data() + 0x5c, 0);         // object+0x64: no upload slot yet (the title's slot allocator
                                          // writes it when a model consumer uploads; TextureUploads binds)
    header[0x62] = 0; header[0x63] = 0;   // object+0x6a: first context
    TextureAssets::View loaded;
    const auto result = assets_.Load(Physical(object), Word(obj + 4), record, size, loaded, error);
    if (result != TextureAssets::Result::Ok) {
        if (result == TextureAssets::Result::InvalidAsset) return Result::InvalidAsset;
        if (result == TextureAssets::Result::UnsupportedAsset) return Result::UnsupportedAsset;
        return Result::ResourceFailure;
    }
    // All memory extents are proven writable, disjoint, and stable. Nothing
    // after publication calls out or can reject one half of this commit.
    std::memcpy(obj + 8, header.data(), header.size());
    Put(cursor, Word(cursor) + uint32_t(size));
    out = std::move(loaded);
    ++counts_.loaded;
    return Result::Ok;
}
TextureBindings::Result TextureBindings::Destroy(uint32_t object, uint32_t flags, const FreeObject& free_object) {
    ps2r::ee::TickProfile::Scope profile(ps2r::ee::TickProfile::Texture);
    auto* obj = RamBytes(object, 0x6c);
    if (!obj || (object & 3)) return Result::InvalidMemory;
    TextureAssets::View view;
    const auto found = assets_.Find(Physical(object), view);
    if (found == TextureAssets::Result::MissingAsset) return Result::NotOwned;
    if (found != TextureAssets::Result::Ok) return Result::ResourceFailure;
    if (view.asset_id != Word(obj + 4) || Word(obj + 0x60) != 0xffffffff) {
        // Same recovery as Read: the original destroys an object it now owns.
        if (assets_.Release(view.token) != TextureAssets::Result::Ok) return Result::ResourceFailure;
        if (released_) released_(view.token);
        return Result::ConflictingOwner;
    }
    if ((flags & 1) && !free_object) return Result::MissingDeallocator;
    if (assets_.Release(view.token) != TextureAssets::Result::Ok) return Result::ResourceFailure;
    if (released_) released_(view.token);
    if (flags & 1) free_object(object);
    ++counts_.destroyed;
    return Result::Ok;
}
GraphicsRefusals::Reason TextureBindings::RefusalReason(Result result) {
    using Reason = GraphicsRefusals::Reason;
    switch (result) {
        case Result::InvalidMemory: case Result::UnsupportedStream: case Result::InvalidStream:
        case Result::InvalidAsset: case Result::UnsupportedAsset: case Result::UnsupportedMaterial:
        case Result::InheritedMaterial: case Result::UnsupportedLod:
            return Reason::UnsupportedState;
        case Result::ConflictingOwner: case Result::MissingDeallocator:
            return Reason::Ordering;
        case Result::ResourceFailure: case Result::NotOwned: case Result::NotSelected: case Result::Ok:
            break;
    }
    return Reason::MissingResource;
}
bool TextureBindings::Install(const Capture& capture, const Installer& install, Deallocate deallocate,
                              GraphicsRefusals::Sink refuse) {
    if (!capture || !install || !selected_ || !deallocate || !Bytes(kMemoryStream, 0xb0)) return false;
    const auto read = capture(kRead), destroy = capture(kDestroy);
    if (!read || !destroy) return false;
    for (const auto function : {kRead, kDestroy}) {
        auto original = function == kRead ? read : destroy;
        if (!install(function, [this, original, function, deallocate, refuse](CpuContext& c, uint32_t entry) {
            ++counts_.calls;
            if (entry && entry != function) {
                // Nothing of the owned read/destroy has started at an interior
                // label: the original continuation owns it (ordering refusal).
                ++counts_.refused;
                if (refuse) refuse(GraphicsRefusals::Reason::Ordering, function, 0x1000);
                original(c, entry);
                return;
            }
            const uint32_t object = c.cpuRegs.GPR.r[4].UL[0], argument = c.cpuRegs.GPR.r[5].UL[0];
            TextureAssets::View view;
            std::string error;
            Result result;
            try {
                result = function == kRead ? Read(object, argument, view, &error) :
                    Destroy(object, argument, [&](uint32_t p) { deallocate(c, p); });
            } catch (...) { ++counts_.failed; throw; }
            if (result == Result::NotSelected || result == Result::NotOwned) {
                ++counts_.declined;
                original(c, entry);
            } else if (result != Result::Ok) {
                // Nothing was committed: Read/Destroy validate every extent and
                // the asset before the single guest write / release. The
                // original owns this object from here on.
                ++counts_.refused;
                if (refuse) refuse(RefusalReason(result), function, uint32_t(result));
                original(c, entry);
            }
        })) return false;
    }
    return true;
}
TextureBindings::Result TextureBindings::CaptureMaterial(uint32_t shader, Material& out) const {
    const auto* s = Bytes(shader, 0x70);
    if (!s || (shader & 3)) return Result::InvalidMemory;
    Material material;
    material.type = Word(s + 0x64);
    bool mips = false, font = false;
    switch (material.type) {
        case 0x01: case 0x02: case 0x04: case 0x15: case 0x16: case 0x17:
        case 0x19: case 0x1a: case 0x1b: case 0x1e: case 0x1f: case 0x20: mips = true; break;
        case 0x14: mips = true; material.wrap = Wrap::Clamp; break;
        case 0x0d: mips = true; font = true; material.wrap = Wrap::Clamp; break;
        case 0x00: case 0x0a: case 0x0c: case 0x0e: case 0x12: case 0x13: break;
        case 0x0f: material.wrap = Wrap::Clamp; break;
        case 0x10: case 0x11: case 0x18:
            material.source = Source::SceneCopy;
            material.wrap = Wrap::Clamp;
            material.alpha = material.type != 0x18;
            break;
        case 0x03: break;
        case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0b: case 0x1c:
            return Result::InheritedMaterial;
        default: return Result::UnsupportedMaterial;
    }
    const auto* table = Bytes(Word(s + 0x6c), 0x30);
    if (!table || (Word(s + 0x6c) & 3) || SignedHalf(table + 0x28) ||
        Word(table + 0x2c) != kSettings[material.type]) return Result::UnsupportedMaterial;
    if (material.type == 0x03) { out = material; return Result::Ok; }
    const uint64_t settings = Word(s) | uint64_t(Word(s + 4)) << 32;
    // Type zero uses the source bit as FST instead of its complement.
    // Screen-copy's zero-initialized primitive leaves FST clear (STQ).
    const bool stq = (settings >> 28) & 1;
    material.stq = material.source == Source::SceneCopy || (material.type == 0 ? !stq : stq);
    if (!font && material.source != Source::SceneCopy && !((settings >> 27) & 1)) {
        out = material;
        return Result::Ok;
    }
    const bool linear = (settings >> 55) & 1;
    material.magnify = font || linear ? Filter::Linear : Filter::Nearest;
    material.minify = linear ? Filter::Linear : Filter::Nearest;
    const int k = SignedHalf(s + 0x18), l = SignedHalf(s + 0x1a);
    // Negative/out-of-range L spills into unrelated old register fields.
    // Decline rather than invent a sane-looking filter for malformed metadata.
    if (k < -2048 || k > 2047 || l < 0 || l > 3) return Result::UnsupportedLod;
    material.lod_bias = int16_t(k);
    material.lod_shift = uint8_t(l);
    if (material.source != Source::SceneCopy) {
        const uint32_t object = Word(s + 0x14);
        const auto* obj = RamBytes(object, 0x6c);
        if (!obj || (object & 3)) return Result::InvalidMemory;
        const auto found = assets_.Find(Physical(object), material.asset);
        if (found == TextureAssets::Result::MissingAsset) return Result::NotOwned;
        if (found != TextureAssets::Result::Ok) return Result::ResourceFailure;
        if (Word(s + 0x60) != material.asset.asset_id || Word(obj + 4) != material.asset.asset_id ||
            Word(obj + 0x60) != 0xffffffff) return Result::ConflictingOwner;
        material.source = Source::Asset;
        material.alpha = material.asset.image->alpha;
        material.function = material.asset.image->function;
        material.max_level = uint8_t(material.asset.image->levels.size() - 1);
        if (mips && material.max_level) material.minify = Filter::LinearMipNearest;
    }
    out = std::move(material);
    return Result::Ok;
}
bool TextureBindings::View(uint32_t object, TextureAssets::View& out) const {
    const auto* obj = RamBytes(object, 0x6c);
    if (!obj || (object & 3) || Word(obj + 0x60) != 0xffffffff) return false;
    TextureAssets::View view;
    if (assets_.Find(Physical(object), view) != TextureAssets::Result::Ok ||
        view.asset_id != Word(obj + 4) || !view.image || view.image->levels.empty()) return false;
    out = std::move(view);
    return true;
}

std::shared_ptr<const TextureCodec::Image> TextureBindings::Converted(uint32_t object) const {
    const auto* obj = RamBytes(object, 0x6c);
    if (!obj || (object & 3) || Word(obj + 0x60) != 0xffffffff) return nullptr;
    TextureAssets::View view;
    if (assets_.Find(Physical(object), view) != TextureAssets::Result::Ok ||
        view.asset_id != Word(obj + 4)) return nullptr;
    return view.image;
}

bool TextureBindings::ValidateSnapshotMemory(const TextureAssets::Snapshot& snapshot, Memory incoming,
                                              const Select& selected, std::string* error) {
    if (!TextureAssets::ValidateSnapshot(snapshot, TextureAssets::Limits{}, error)) return false;
    const auto bad = [&](const char* why) {
        if (error) *error = std::string("Texture state RAM: ") + why;
        return false;
    };
    if (!incoming.data || incoming.size != 0x02000000 || !selected)
        return bad("requires the complete incoming RAM image and an explicit selection");
    for (const auto& asset : snapshot.assets) {
        const auto object = asset.token.object;
        if (object >= incoming.size || 0x6c > incoming.size - object)
            return bad("object outside incoming physical RAM");
        const auto* obj = incoming.data + object;
        if (!selected(object, asset.asset_id)) return bad("serialized owner is not selected");
        if (Word(obj + 4) != asset.asset_id || Word(obj + 0x60) != 0xffffffff)
            return bad("source identity or transfer ownership disagrees");
        // TextureRead copies the size prefix at +8 and asset header at +0x0c.
        const auto* h = obj + 0x0c;
        const auto width = unsigned(h[4]) | unsigned(h[5]) << 8;
        const auto height = unsigned(h[6]) | unsigned(h[7]) << 8;
        const auto& image = asset.image;
        if (width > 10 || height > 10 || (1u << width) != image.levels[0].width ||
            (1u << height) != image.levels[0].height || h[8] != image.levels.size() ||
            h[9] != (image.format == TextureCodec::Format::Indexed8 ? 0x13 : 0) ||
            h[10] || h[11] != unsigned(image.alpha) || h[13] != image.function)
            return bad("source dimensions, format, palette mode or colour metadata disagrees");
    }
    if (error) error->clear();
    return true;
}

}  // namespace title
