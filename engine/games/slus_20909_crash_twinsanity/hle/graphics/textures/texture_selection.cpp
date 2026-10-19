#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_selection.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_consumers.h"

namespace title {
namespace {
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t Half(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t Physical(uint32_t address) { return address & 0x1fffffff; }
[[noreturn]] void Refuse(const char* why) { throw std::runtime_error(std::string("Texture selection: ") + why); }
bool ValidExtent(TextureBindings::Memory memory, uint32_t address, size_t size) {
    const auto segment = address & 0xe0000000, at = Physical(address);
    return memory.data && !(address & 3) && at && at < 0x02000000 && at < memory.size &&
        (segment == 0 || segment == 0x80000000 || segment == 0xa0000000) &&
        size <= 0x02000000 - at && size <= memory.size - at;
}
const uint8_t* MemoryBytes(TextureBindings::Memory memory, uint32_t address, size_t size) {
    if (!ValidExtent(memory, address, size)) Refuse("source object extent/segment is invalid");
    return memory.data + Physical(address);
}
GraphicsRefusals::Reason DeclineReason(TextureSelection::Decline code) {
    using Decline = TextureSelection::Decline;
    switch (code) {
        case Decline::NoTextureObject: case Decline::MissingAsset: case Decline::LostOwner:
            return GraphicsRefusals::Reason::MissingResource;
        case Decline::InvalidExtent: case Decline::EmptyMaterial: case Decline::EmptyFont:
        case Decline::ResourceElsewhere: case Decline::Unselected:
            break;
    }
    return GraphicsRefusals::Reason::UnsupportedState;
}
constexpr uint64_t Shader(unsigned type) { return uint64_t(1) << type; }
constexpr uint32_t kNoRecord = 0xffff;
std::vector<TextureSelection::Left> Sorted(const std::map<uint32_t, uint64_t>& ledger) {
    std::vector<TextureSelection::Left> rows;
    for (const auto& [texture, count] : ledger) {
        const auto* entry = TextureConsumers::Find(texture);
        rows.push_back({texture, entry ? entry->families : kNoRecord, count});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
    return rows;
}
}

TextureSelection::TextureSelection(const TextureAssets& assets, TextureBindings::Memory memory, Services ready,
                                   GraphicsRefusals* refusals)
    : assets_(assets), memory_(memory), ready_(ready), refusals_(refusals) {}

// The UI pair's kind names its producer; TextureConsumers decides whether
// that producer is the texture's ONLY consumer (families AND shader types):
// a picture a rigid model also reads, or whose ID word sits in an unclassified
// payload, is Retained and its sprite stays with the original. Fonts: the
// three Crash pages; Arial stays deferred until its caller is identified
//The archive identity is the retail one the catalogues
// were generated from (TextureOwners::kArchiveHeaderSha256).
bool TextureSelection::Selected(uint32_t asset_id, Services ready) {
    const auto* pair = TexturePairs::Find(asset_id);
    if (!pair) return false;
    uint32_t families = 0;
    uint64_t shaders = 0;
    if (ready.sprites) { families |= TextureConsumers::Sprite; shaders |= Shader(0x0d) | Shader(0x0e); }
    if (ready.text) { families |= TextureConsumers::Font; shaders |= Shader(0x0d); }
    if (pair->kind == TexturePairs::Kind::Font) {
        const auto* startup = TextureOwners::Find(asset_id);
        if (!ready.text || !startup || startup->family != TextureOwners::Family::CrashFont) return false;
    } else if (!ready.sprites) {
        return false;
    }
    const auto readiness = TextureConsumers::Inspect(TextureConsumers::RetailArchive(), asset_id, families, shaders);
    return readiness.result == TextureConsumers::Result::Owned;
}
bool TextureSelection::Texture(uint32_t object, uint32_t asset_id) const {
    if (ConvertedId(asset_id, ready_)) return true;
    if (object) ++declined_textures_[asset_id]; // a load this owner left to the original (policy probes pass 0)
    return false;
}
const uint8_t* TextureSelection::Bytes(uint32_t address, size_t size) const {
    return MemoryBytes(memory_, address, size);
}
bool TextureSelection::Declined(Consumer consumer, Decline code, uint32_t object) const {
    if (refusals_)
        refusals_->Refuse(consumer == Consumer::Font ? GraphicsRefusals::Family::Text : GraphicsRefusals::Family::Sprites,
                          DeclineReason(code), object, uint32_t(code));
    return false;
}
// declined is set when this probe saw a selected identity that this owner does
// not hold (nothing converted): the consumer must stay with the original.
const TexturePairs::Entry* TextureSelection::Probe(uint32_t object, uint32_t declared_id, Decline* why) const {
    if (!object) {
        if (Selected(declared_id, ready_)) *why = Decline::MissingAsset;
        return nullptr;
    }
    const auto* obj = Bytes(object, 0x6c);
    const auto id = Word(obj + 4);
    TextureAssets::View view;
    const auto found = assets_.Find(Physical(object), view);
    const bool expected = Selected(id, ready_) || Selected(declared_id, ready_);
    if (found == TextureAssets::Result::MissingAsset) {
        if (expected) *why = Decline::MissingAsset; // the original loaded it (this owner's Read refused)
        return nullptr;
    }
    if (found != TextureAssets::Result::Ok) Refuse("selected texture is stale or failed");
    if (!Selected(id, ready_)) {
        // Converted at load (the `textures` service) but not this producer's to
        // draw: the original draws it from the slot bind. A decline, not a fault.
        if (!ready_.textures) Refuse("live texture ownership disagrees with selected services");
        *why = Decline::Unselected;
        return nullptr;
    }
    if (id != declared_id || view.asset_id != id || Word(obj + 0x60) != 0xffffffff)
        Refuse("shader, source object and converted owner identities disagree");
    return TexturePairs::Find(id);
}
TextureSelection::Owner TextureSelection::Material(
    uint32_t material, uint32_t resource, Consumer consumer, bool& declined) const {
    Owner result;
    if (!ValidExtent(memory_, material, 0x24)) { Declined(consumer, Decline::InvalidExtent, material); declined = true; return result; }
    const auto* m = Bytes(material, 0x24);
    const auto count = Word(m + 0x10);
    if (!count) { Declined(consumer, Decline::EmptyMaterial, material); declined = true; return result; }
    if (count > 4) Refuse("material shader count is not source-classified");
    uint32_t resource_id = 0;
    bool expected_resource = false, elsewhere = false;
    if (resource) {
        if (!ValidExtent(memory_, resource, 12)) { Declined(consumer, Decline::InvalidExtent, resource); declined = true; return result; }
        const auto* r = Bytes(resource, 12);
        elsewhere = Physical(Word(r + 8)) != Physical(material);
        resource_id = Word(r + 4);
        if (const auto* pair = TexturePairs::FindMaterial(resource_id); pair && Selected(pair->texture, ready_))
            expected_resource = true;
    }
    const auto kind = consumer == Consumer::Font ? TexturePairs::Kind::Font : TexturePairs::Kind::Picture;
    bool missing = false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto shader_address = Word(m + i * 4);
        if (!ValidExtent(memory_, shader_address, 0x70)) { Declined(consumer, Decline::InvalidExtent, shader_address); declined = true; return result; }
        const auto* shader = Bytes(shader_address, 0x70);
        const auto object = Word(shader + 0x14);
        Decline why = Decline(0);
        const auto* owner = Probe(object, Word(shader + 0x60), &why);
        if (why != Decline(0)) {
            Declined(consumer, !object ? Decline::NoTextureObject : why, object ? object : shader_address);
            missing = true;
        }
        if (!owner) continue;
        // From here an OWNED texture is involved: the original has no upload
        // for it, so an unclassified consumer is a fault, never a fallback.
        // The pair catalogue proves every PSM/PSF material is one type-0D
        // shader with program key 0x10000000 in bucket 24/26 (audit_texture_pairs.py).
        if (elsewhere) Refuse("material resource points elsewhere");
        if (count != 1 || owner->kind != kind || Word(shader + 0x64) != 0x0d ||
            Word(m + 0x18) != 0x10000000 || Word(m + 0x1c) != 0 ||
            Word(m + 0x20) != (kind == TexturePairs::Kind::Picture ? 24u : 26u))
            Refuse("selected texture reached an unclassified material consumer");
        if (resource && resource_id != owner->material) Refuse("selected material/texture pair differs");
        result = {owner, Physical(object)};
    }
    if (result.pair && missing) Refuse("selected material mixes owned and original textures");
    if (!result.pair) {
        if (missing) declined = true;
        else if (elsewhere) { Declined(consumer, Decline::ResourceElsewhere, resource); declined = true; }
        else if (expected_resource) { Declined(consumer, Decline::LostOwner, material); declined = true; }
    }
    return result;
}
// The untextured flat Sprite (TR materials.cpp MakeFlatMaterial: one type-0E
// shader, textured bit clear, no texture object; bucket 24): no texture to
// own, the owned producer draws it from the material alone. The textured 0x0E
// variant (OLEG's 2D particle sparkles) samples the particle page and stays
// with the original until that page has an owner.
bool TextureSelection::FlatSprite(uint32_t material) const {
    if (!ValidExtent(memory_, material, 0x24)) return false;
    const auto* m = Bytes(material, 0x24);
    const auto shader_address = Word(m);
    if (Word(m + 0x10) != 1 || Word(m + 0x20) != 24 || !ValidExtent(memory_, shader_address, 0x70)) return false;
    const auto* shader = Bytes(shader_address, 0x70);
    return Word(shader + 0x64) == 0x0e && !Word(shader + 0x14) && !((Word(shader) >> 27) & 1);
}
void TextureSelection::LeaveSprite(uint32_t material) const {
    uint32_t texture = 0;
    if (ValidExtent(memory_, material, 0x24)) {
        const auto* m = Bytes(material, 0x24);
        const auto shader_address = Word(m);
        if (Word(m + 0x10) && ValidExtent(memory_, shader_address, 0x70)) {
            const auto* shader = Bytes(shader_address, 0x70);
            const auto object = Word(shader + 0x14);
            texture = Word(shader + 0x60);
            if (object && ValidExtent(memory_, object, 0x6c)) texture = Word(Bytes(object, 0x6c) + 4);
        }
    }
    ++unselected_sprites_[texture];
}
bool TextureSelection::Sprite(uint32_t sprite) const {
    if (!ValidExtent(memory_, sprite, 0x20)) {
        ++unselected_sprites_[0];
        return Declined(Consumer::Sprite, Decline::InvalidExtent, sprite);
    }
    const auto* source = Bytes(sprite, 0x20);
    const auto material = Word(source);
    bool declined = false;
    if (Material(material, Word(source + 4), Consumer::Sprite, declined).pair) return true;
    if (!declined && ready_.sprites && FlatSprite(material)) return true;
    LeaveSprite(material);
    return false;
}
bool TextureSelection::Font(uint32_t font) const {
    constexpr auto consumer = Consumer::Font;
    if (!ValidExtent(memory_, font, 0x34)) return Declined(consumer, Decline::InvalidExtent, font);
    const auto* source = Bytes(font, 0x34);
    const auto count = Word(source + 0x18);
    if (!count) return Declined(consumer, Decline::EmptyFont, font);
    if (count > 3) Refuse("font page count is not source-classified");
    bool selected = false, retained = false, declined = false;
    constexpr uint32_t pages[]{0x922ed81e, 0x96432c5c, 0x9a57809a};
    for (uint32_t i = 0; i < count; ++i) {
        const auto resource = Word(source + 0x1c + i * 4);
        if (!ValidExtent(memory_, resource, 12)) return Declined(consumer, Decline::InvalidExtent, resource);
        const auto* r = Bytes(resource, 12);
        const auto material = Material(Word(r + 8), resource, consumer, declined);
        const auto object = Word(source + 0x28 + i * 4);
        const uint8_t* texture = nullptr;
        if (object) {
            if (!ValidExtent(memory_, object, 0x6c)) return Declined(consumer, Decline::InvalidExtent, object);
            texture = Bytes(object, 0x6c);
        }
        Decline why = Decline(0);
        const auto* owner = texture ? Probe(object, Word(texture + 4), &why) : nullptr;
        if (why != Decline(0)) { Declined(consumer, why, object); declined = true; }
        if (material.pair || owner) {
            if (!material.pair || !owner || material.pair != owner ||
                material.object != Physical(object) || count != 3 || owner->texture != pages[i])
                Refuse("selected font has mismatched, missing or reordered pages");
            selected = true;
        } else retained = true;
    }
    if (selected && (retained || declined)) Refuse("mixed font ownership cannot delegate");
    return selected;
}
std::vector<TextureSelection::Left> TextureSelection::UnselectedSprites() const { return Sorted(unselected_sprites_); }
std::vector<TextureSelection::Left> TextureSelection::DeclinedTextures() const { return Sorted(declined_textures_); }

bool TextureSelection::PreflightSnapshot(const TextureAssets::Snapshot& snapshot,
    TextureBindings::Memory incoming, Services ready, std::string* error) {
    const auto selection = [ready](uint32_t, uint32_t id) { return ConvertedId(id, ready); };
    if (!TextureBindings::ValidateSnapshotMemory(snapshot, incoming, selection, error)) return false;
    try {
        const auto* table = MemoryBytes(incoming, kTextureTable, 0x24);
        // Before the title constructs its TextureTable (boot: ConstructBase
        // has not run) the object is all zero and nothing can be owned yet:
        // a consistent image carries no TXAS assets and pairs nothing.
        if (std::all_of(table, table + 0x24, [](uint8_t b) { return b == 0; })) {
            if (!snapshot.assets.empty()) Refuse("TXAS carries assets but the incoming TextureTable is not constructed");
            if (error) error->clear();
            return true;
        }
        const auto count = Half(table + 4), capacity = Half(table + 6);
        // The deletion queue is the title's static g_TextureQueue from boot
        // (ConstructBase) or none (Construct alone); any other pointer is an
        // unclassified owner. Queued objects are released but still allocated:
        // they pair with TXAS exactly like table rows (their deletion runs
        // through the TextureDestroy adapter).
        const auto queue = Word(table + 0x10);
        if (Word(table + 0x14) != kTableVtable || Half(table + 8) != 0x40 ||
            (queue && queue != kTextureQueue) || count > capacity)
            Refuse("incoming TextureTable has an unclassified owner");
        const auto* methods = MemoryBytes(incoming, kTableVtable, 0x28);
        if (Half(methods + 0x20) || Word(methods + 0x24) != 0x1c1c78)
            Refuse("incoming TextureTable acquisition boundary differs");
        std::unordered_map<uint32_t, uint32_t> remaining;
        for (const auto& asset : snapshot.assets) remaining.emplace(asset.token.object, asset.asset_id);
        const auto pair = [&](uint32_t object, uint32_t id) {
            const auto* obj = MemoryBytes(incoming, object, 0x6c);
            if (Word(obj + 4) != id) Refuse("incoming table and object IDs disagree");
            const auto at = remaining.find(Physical(object));
            // Under `textures` the ownership word decides: a converted object
            // carries no DiskManager transfer; one this owner refused (an
            // unconvertible record, the byte limit) was loaded by the original.
            const bool owned = ready.textures ? Word(obj + 0x60) == 0xffffffff : Selected(id, ready);
            if (owned) {
                if (at == remaining.end() || at->second != id) Refuse("selected live texture is absent from TXAS");
                remaining.erase(at);
            } else if (at != remaining.end()) Refuse("TXAS includes an unselected table owner");
        };
        const auto entries = Word(table);
        if ((!entries && capacity) || (entries && !capacity)) Refuse("incoming TextureTable allocation differs");
        const uint8_t* rows = entries ? MemoryBytes(incoming, entries, size_t(capacity) * 8) : nullptr;
        uint32_t previous = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto object = Word(rows + i * 8), id = Word(rows + i * 8 + 4);
            if (i && id >= previous) Refuse("incoming TextureTable is not in unique descending ID order");
            previous = id;
            if (!object) continue; // source Acquire creates a missing entry's object later
            pair(object, id);
        }
        if (queue) {
            const auto* q = MemoryBytes(incoming, queue, 4 + size_t(kQueueItems) * 4);
            const auto head = Half(q), queued = Half(q + 2);
            if (head >= kQueueItems || queued > kQueueItems) Refuse("incoming TextureTable deletion queue is malformed");
            for (uint32_t i = 0; i < queued; ++i) {
                const auto item = Word(q + 4 + ((head + i) % kQueueItems) * 4);
                if (!item) continue; // DeleteFirst skips a null item
                pair(item, Word(MemoryBytes(incoming, item, 0x6c) + 4));
            }
        }
        if (!remaining.empty()) Refuse("TXAS owner is absent from incoming TextureTable");
    } catch (const std::runtime_error& failure) {
        if (error) *error = failure.what();
        return false;
    }
    if (error) error->clear();
    return true;
}

}  // namespace title
