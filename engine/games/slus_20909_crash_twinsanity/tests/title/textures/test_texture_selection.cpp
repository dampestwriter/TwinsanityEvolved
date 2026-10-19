#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>

#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_consumers.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pairs.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_selection.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_state_codec.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"

namespace {
using namespace title;
using F = TextureBindingFixture;
using Policy = TextureSelection;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)
constexpr uint32_t sprite = 0x140000, material = 0x150000, font = 0x160000, resource = 0x170000;
constexpr uint32_t hud = 0x046a8347;
constexpr uint32_t pages[]{0x922ed81e, 0x96432c5c, 0x9a57809a};
bool Refused(const std::function<void()>& call) {
    try { call(); } catch (const std::runtime_error&) { return true; }
    return false;
}
void SpriteSource(F& f, uint32_t id) {
    f.Put(F::object + 4, id);
    f.Shader(13);
    f.Put(F::shader + 0x60, id);
    f.Put(sprite, material); f.Put(sprite + 4, resource); f.Put(sprite + 0xc, 0x2f6b50);
    f.Put(material, F::shader); f.Put(material + 0x10, 1);
    f.Put(material + 0x18, 0x10000000); f.Put(material + 0x20, 24);
    const auto* owner = TextureOwners::Find(id);
    f.Put(resource + 4, owner ? owner->material : 0x12345678);
    f.Put(resource + 8, material);
}
void Load(F& f, TextureBindings& bindings, uint32_t object, uint32_t id) {
    f.Put(object + 4, id); f.Put(object + 0x60, 0xffffffff);
    f.Put(F::stream + 12, F::data);
    TextureAssets::View out;
    CHECK(bindings.Read(object, F::stream, out) == TextureBindings::Result::Ok);
}
void FontSource(F& f, TextureBindings& bindings) {
    f.Shader(13, -16, 0, true, false);
    f.Put(font + 0x18, 3);
    for (unsigned i = 0; i < 3; ++i) {
        const auto object = F::object + i * 0x100, shader = F::shader + i * 0x100;
        Load(f, bindings, object, pages[i]);
        if (i) std::memcpy(f.ram.data() + shader, f.ram.data() + F::shader, 0x70);
        f.Put(shader + 0x14, object); f.Put(shader + 0x60, pages[i]);
        f.Put(material + i * 0x100, shader);
        f.Put(material + i * 0x100 + 0x10, 1);
        f.Put(material + i * 0x100 + 0x18, 0x10000000);
        f.Put(material + i * 0x100 + 0x20, 26);
        f.Put(resource + i * 0x10 + 4, TextureOwners::Find(pages[i])->material);
        f.Put(resource + i * 0x10 + 8, material + i * 0x100);
        f.Put(font + 0x1c + i * 4, resource + i * 0x10);
        f.Put(font + 0x28 + i * 4, object);
    }
}
void SpriteSelection() {
    F f;
    GraphicsRefusals ledger;
    Policy policy(f.assets, {f.ram.data(), f.ram.size()}, {true, true}, &ledger);
    const auto declines = [&] { return ledger.Of(GraphicsRefusals::Family::Sprites); };
    TextureBindings bindings(f.assets, {f.ram.data(), f.ram.size()},
        [&](uint32_t object, uint32_t id) { return policy.Texture(object, id); });
    unsigned selected = 0;
    for (const auto& entry : TextureOwners::kStartup) selected += policy.Texture(F::object, entry.texture);
    CHECK(selected == 43 && !policy.Texture(F::object, 0x19235bb6) && !policy.Texture(F::object, 19));
    // The ledger names what the policy left to the original (Arial, the unknown id).
    CHECK(policy.DeclinedTextures().size() == 2 && policy.DeclinedTextures()[0].texture == 0x19235bb6 &&
          policy.DeclinedTextures()[0].count == 2 && policy.DeclinedTextures()[1].count == 1 &&
          !policy.Texture(0, 19) && policy.DeclinedTextures()[1].count == 1); // probes with object 0 are not loads
    SpriteSource(f, 19); // includes OLEG's borrowed world-material Sprite case
    CHECK(!policy.Sprite(sprite));
    f.Put(sprite + 4, 0);
    CHECK(!policy.Sprite(sprite)); // null MaterialResource is not an ownership marker
    SpriteSource(f, hud);
    // Expected but never converted by this owner (its Read refused, the
    // original loaded it): a counted DECLINE to the original, not a fault.
    CHECK(!policy.Sprite(sprite) && declines().total == 1);
    CHECK(declines().first.detail == uint32_t(Policy::Decline::MissingAsset) &&
          declines().first.reason == GraphicsRefusals::Reason::MissingResource &&
          declines().first.producer == F::object);
    CHECK(ledger.Of(GraphicsRefusals::Family::Text).total == 0);
    Load(f, bindings, F::object, hud);
    CHECK(policy.Sprite(sprite) && policy.Sprite(sprite | 0xa0000000));
    f.Put(sprite + 4, 0);
    CHECK(policy.Sprite(sprite)); // direct material alias must use the owned texture
    f.Put(sprite + 4, resource);
    Policy disabled(f.assets, {f.ram.data(), f.ram.size()}, {});
    CHECK(!disabled.Texture(F::object, hud) && Refused([&] { disabled.Sprite(sprite); }));
    // An OWNED texture reached by an inconsistent/unclassified consumer: the
    // original has no upload for it, so this stays a fault (no silent fallback).
    for (const auto& pair : {std::pair<uint32_t, uint32_t>{resource + 4, 0},
            {resource + 8, material + 4}, {F::shader + 0x60, 19},
            {F::shader + 0x64, 14}, {F::object + 4, 19}, {F::object + 0x60, 2},
            {material + 0x20, 26}, {material + 0x18, 0}}) {
        const auto before = f.Word(pair.first); f.Put(pair.first, pair.second);
        CHECK(Refused([&] { policy.Sprite(sprite); }));
        f.Put(pair.first, before);
    }
    // A selected shader id without a texture object references nothing this
    // owner holds: declined (NoTextureObject), the original draws it.
    f.Put(F::shader + 0x14, 0);
    CHECK(!policy.Sprite(sprite) && declines().total == 2 &&
          declines().last.detail == uint32_t(Policy::Decline::NoTextureObject));
    f.Put(F::shader + 0x14, F::object);
    CHECK(policy.Sprite(sprite));
    // An owned texture in a later shader cannot hide behind a retained first
    // shader. The complete material is not implemented by the first-shader UI path.
    std::memcpy(f.ram.data() + F::shader + 0x100, f.ram.data() + F::shader, 0x70);
    f.Put(material + 4, F::shader + 0x100); f.Put(material + 0x10, 2);
    f.Put(F::shader + 0x14, 0); f.Put(F::shader + 0x60, 0);
    CHECK(Refused([&] { policy.Sprite(sprite); }));
    SpriteSource(f, hud);
    // An unreadable consumer object is declined with InvalidExtent (the gate-4
    // shape at the consumer side); the original handles its own pointer.
    CHECK(!policy.Sprite(0xffffffff) && declines().total == 3 &&
          declines().last.detail == uint32_t(Policy::Decline::InvalidExtent) &&
          declines().last.reason == GraphicsRefusals::Reason::UnsupportedState);
    CHECK(!policy.Sprite(sprite + 1) && declines().total == 4);
    CHECK(f.queue.BeginFrame(1) == Graphics::Result::Ok);
    TextureAssets::View view;
    CHECK(f.assets.Find(F::object, view) == TextureAssets::Result::Ok);
    CHECK(f.assets.Record(f.queue.End(24), view.token) == TextureAssets::Result::Ok);
    f.backend.fail_mip = true;
    Graphics::Hooks hooks;
    hooks.upload = [&](const Graphics::Upload& upload) { return f.assets.Upload(upload, false) == TextureAssets::Result::Ok; };
    hooks.drain = [] { return true; };
    CHECK(f.queue.Submit(hooks) == Graphics::Result::DispatchFailed);
    CHECK(Refused([&] { policy.Sprite(sprite); })); // owned but failed: no arm can draw it
    CHECK(f.queue.Recover(hooks) == Graphics::Result::Ok);
    CHECK(declines().total == 4); // faults are not declines
}

// The widened UI selection. Every PSM picture whose only retail
// consumer family is Sprite (TextureConsumers Owned under Sprite with shaders
// 0x0D/0x0E) is selected under `sprites`; the pictures an unownable family
// also references (the nine Extras gallery opaque references) and every font
// page under `sprites` alone are not; the startup catalogue is a subset of the
// pair catalogue with the same materials; the untextured flat Sprite selects
// without a texture while the textured 0x0E particle variant does not.
void WidenedSelection() {
    size_t pictures = 0, fonts = 0, owned = 0, retained = 0, startup_pairs = 0;
    uint32_t first_retained = 0;
    for (size_t i = 0; i < TexturePairs::Size(); ++i) {
        const auto* pair = TexturePairs::At(i);
        CHECK(pair && (i == 0 || TexturePairs::At(i - 1)->texture < pair->texture));
        CHECK(TexturePairs::Find(pair->texture) == pair && TexturePairs::FindMaterial(pair->material) == pair);
        const bool sprites_select = Policy::SelectedId(pair->texture, {false, true});
        const bool text_select = Policy::SelectedId(pair->texture, {true, false});
        if (pair->kind == TexturePairs::Kind::Font) {
            ++fonts;
            CHECK(!sprites_select);
            CHECK(text_select == (pair->texture != 0x19235bb6)); // Arial deferred
            continue;
        }
        ++pictures;
        const auto ready = TextureConsumers::Inspect(TextureConsumers::RetailArchive(), pair->texture,
                                                     TextureConsumers::Sprite, (1ull << 0x0d) | (1ull << 0x0e));
        CHECK(ready.result == TextureConsumers::Result::Owned || ready.result == TextureConsumers::Result::Retained);
        CHECK(sprites_select == (ready.result == TextureConsumers::Result::Owned) && !text_select);
        if (sprites_select) ++owned; else { ++retained; if (!first_retained) first_retained = pair->texture; }
        if (ready.result == TextureConsumers::Result::Retained)
            CHECK((ready.retained_families & TextureConsumers::Unownable) && !ready.unsupported_shaders);
    }
    CHECK(pictures == 2578 && fonts == 4 && owned == 2569 && retained == 9);
    CHECK(TexturePairs::Size() == 2582 && !TexturePairs::Find(19) && !TexturePairs::FindMaterial(19));
    for (const auto& entry : TextureOwners::kStartup) {
        const auto* pair = TexturePairs::Find(entry.texture);
        startup_pairs += pair && pair->material == entry.material &&
            pair->kind == (entry.family == TextureOwners::Family::Hud ? TexturePairs::Kind::Picture : TexturePairs::Kind::Font);
    }
    CHECK(startup_pairs == TextureOwners::kStartup.size());
    CHECK(std::string(TexturePairs::ArchiveHeaderSha256()) == TextureOwners::kArchiveHeaderSha256 &&
          std::string(TexturePairs::ArchiveHeaderSha256()) == TextureConsumers::RetailArchive().header_sha256);
    // A widened picture through the live adapters: owned at load, its Sprite
    // selected with the pair's material; a retained gallery picture declined
    // at load, its Sprite left to the original and named in the ledger.
    F f;
    GraphicsRefusals ledger;
    Policy policy(f.assets, {f.ram.data(), f.ram.size()}, {true, true}, &ledger);
    TextureBindings bindings(f.assets, {f.ram.data(), f.ram.size()},
        [&](uint32_t object, uint32_t id) { return policy.Texture(object, id); });
    uint32_t picture = 0;
    for (size_t i = 0; i < TexturePairs::Size() && !picture; ++i)
        if (const auto* pair = TexturePairs::At(i); pair->kind == TexturePairs::Kind::Picture &&
            !TextureOwners::Find(pair->texture) && Policy::SelectedId(pair->texture, {false, true}))
            picture = pair->texture;
    CHECK(picture && !TextureOwners::Find(picture));
    SpriteSource(f, picture);
    f.Put(resource + 4, TexturePairs::Find(picture)->material);
    CHECK(!policy.Sprite(sprite) && ledger.Of(GraphicsRefusals::Family::Sprites).total == 1); // not yet converted: declined
    Load(f, bindings, F::object, picture);
    CHECK(policy.Sprite(sprite) && policy.UnselectedSprites().size() == 1);
    f.Put(resource + 4, TexturePairs::Find(picture)->material ^ 1);
    CHECK(Refused([&] { policy.Sprite(sprite); })); // the pair's material is the only classified consumer
    f.Put(resource + 4, TexturePairs::Find(picture)->material);
    F retained_fixture;
    Policy retained_policy(retained_fixture.assets, {retained_fixture.ram.data(), retained_fixture.ram.size()}, {true, true});
    TextureBindings retained_bindings(retained_fixture.assets, {retained_fixture.ram.data(), retained_fixture.ram.size()},
        [&](uint32_t object, uint32_t id) { return retained_policy.Texture(object, id); });
    retained_fixture.Put(F::object + 4, first_retained); retained_fixture.Put(F::object + 0x60, 0xffffffff);
    retained_fixture.Put(F::stream + 12, F::data);
    TextureAssets::View out;
    CHECK(retained_bindings.Read(F::object, F::stream, out) == TextureBindings::Result::NotSelected);
    SpriteSource(retained_fixture, first_retained);
    retained_fixture.Put(resource + 4, TexturePairs::Find(first_retained)->material);
    CHECK(!retained_policy.Sprite(sprite) && !retained_policy.Sprite(sprite));
    const auto left = retained_policy.UnselectedSprites();
    CHECK(left.size() == 1 && left[0].texture == first_retained && left[0].count == 2 &&
          (left[0].families & TextureConsumers::OpaqueReference) && (left[0].families & TextureConsumers::Sprite));
    const auto declined = retained_policy.DeclinedTextures();
    CHECK(declined.size() == 1 && declined[0].texture == first_retained && declined[0].count == 1);
    // The untextured flat Sprite (one type-0E shader, textured bit clear, no
    // texture object, bucket 24) needs no texture; textured 0x0E does not select.
    F flat;
    Policy flat_policy(flat.assets, {flat.ram.data(), flat.ram.size()}, {true, true});
    flat.Shader(14, -69, 0, true, true, false);
    flat.Put(F::shader + 0x14, 0); flat.Put(F::shader + 0x60, 0);
    flat.Put(sprite, material); flat.Put(sprite + 4, 0); flat.Put(sprite + 0xc, 0x2f6b50);
    flat.Put(material, F::shader); flat.Put(material + 0x10, 1); flat.Put(material + 0x18, 2); flat.Put(material + 0x20, 24);
    CHECK(flat_policy.Sprite(sprite) && flat_policy.UnselectedSprites().empty());
    Policy flat_text_only(flat.assets, {flat.ram.data(), flat.ram.size()}, {true, false});
    CHECK(!flat_text_only.Sprite(sprite));
    flat.Put(F::shader, uint64_t(1) << 27 | uint64_t(1) << 28, 8); // textured 0x0E: the particle page, retained
    CHECK(!flat_policy.Sprite(sprite) && flat_policy.UnselectedSprites().size() == 1 &&
          flat_policy.UnselectedSprites()[0].texture == 0);
    flat.Put(F::shader, 0, 8);
    flat.Put(material + 0x20, 22);
    CHECK(!flat_policy.Sprite(sprite)); // a particle-bucket flat material is not the UI's
    flat.Put(material + 0x20, 24); flat.Put(material + 0x10, 2);
    CHECK(!flat_policy.Sprite(sprite));
}
void FontSelection() {
    F f;
    Policy policy(f.assets, {f.ram.data(), f.ram.size()}, {true, true});
    TextureBindings bindings(f.assets, {f.ram.data(), f.ram.size()},
        [&](uint32_t object, uint32_t id) { return policy.Texture(object, id); });
    FontSource(f, bindings);
    CHECK(policy.Font(font));
    Policy sprites_only(f.assets, {f.ram.data(), f.ram.size()}, {false, true});
    CHECK(Refused([&] { sprites_only.Font(font); }));
    for (const auto& pair : {std::pair<uint32_t, uint32_t>{font + 0x18, 2},
            {font + 0x28, F::object + 0x100}, {font + 0x1c, resource + 0x10},
            {resource + 4, TextureOwners::Find(pages[1])->material},
            {material + 0x20, 24}, {font + 0x30, 0}}) {
        const auto before = f.Word(pair.first); f.Put(pair.first, pair.second);
        CHECK(Refused([&] { policy.Font(font); }));
        f.Put(pair.first, before);
    }
    TextureAssets::View view;
    CHECK(f.assets.Find(F::object + 0x200, view) == TextureAssets::Result::Ok);
    CHECK(f.assets.Release(view.token) == TextureAssets::Result::Ok);
    CHECK(Refused([&] { policy.Font(font); })); // a mixed/missing last page cannot delegate
    F retained;
    SpriteSource(retained, 0x19235bb6);
    retained.Put(material + 0x20, 26);
    retained.Put(font + 0x18, 1); retained.Put(font + 0x1c, resource); retained.Put(font + 0x28, F::object);
    Policy arial(retained.assets, {retained.ram.data(), retained.ram.size()}, {true, true});
    CHECK(!arial.Font(font));
}
void ServiceFailure() {
    F f;
    SpriteSource(f, hud);
    GraphicsRefusals ledger;
    Policy policy(f.assets, {f.ram.data(), f.ram.size()}, {true, true}, &ledger);
    TextureBindings bindings(f.assets, {f.ram.data(), f.ram.size()},
        [&](uint32_t object, uint32_t id) { return policy.Texture(object, id); });
    Load(f, bindings, F::object, hud);
    SpriteService::Hooks hooks;
    hooks.enabled = true;
    hooks.selected = [&](uint32_t object) { return policy.Sprite(object); };
    hooks.refuse = ledger.Refuser(GraphicsRefusals::Family::Sprites);
    hooks.fault = ledger.Faulter(GraphicsRefusals::Family::Sprites);
    hooks.read = [&](uint32_t address, void* out, size_t size) {
        const auto at = address & 0x1fffffff;
        if (at > f.ram.size() || size > f.ram.size() - at) return false;
        std::memcpy(out, f.ram.data() + at, size); return true;
    };
    hooks.environment = [](MaterialState::Environment&) { return true; };
    hooks.record = [](const SpriteIntent::Output&) { return false; };
    unsigned model_calls = 0;
    std::map<uint32_t, SpriteService::Body> entries;
    SpriteService service(f.bindings, hooks);
    CHECK(service.Install([&](uint32_t) -> SpriteService::Body {
        return [&](CpuContext&, uint32_t) { ++model_calls; };
    }, [&](uint32_t address, SpriteService::Body body) { entries[address] = std::move(body); return true; }));
    CpuContext c{};
    c.cpuRegs.GPR.r[4].UL[0] = sprite;
    // This fixture has no source frame/target words: the selected capture
    // refuses (counted, attributed with its capture result) and the original runs.
    entries.at(SpriteIntent::kPlain)(c, 0);
    CHECK(model_calls == 1 && service.Counts().model_calls == 1 && service.Counts().failed == 0);
    CHECK(service.Counts().refused == 1 && ledger.Of(GraphicsRefusals::Family::Sprites).total == 1);
    CHECK(ledger.Of(GraphicsRefusals::Family::Sprites).first.reason == GraphicsRefusals::Reason::UnsupportedState &&
          (ledger.Of(GraphicsRefusals::Family::Sprites).first.detail & SpriteService::kCapture));
    // An owned texture whose consumer is inconsistent stays a fault.
    f.Put(F::shader + 0x64, 14);
    CHECK(Refused([&] { entries.at(SpriteIntent::kPlain)(c, 0); }));
    CHECK(model_calls == 1 && service.Counts().failed == 1 &&
          ledger.Of(GraphicsRefusals::Family::Sprites).faults == 1);
}
void CheckpointClosure() {
    F f;
    Policy policy(f.assets, {f.ram.data(), f.ram.size()}, {true, true});
    TextureBindings bindings(f.assets, {f.ram.data(), f.ram.size()},
        [&](uint32_t object, uint32_t id) { return policy.Texture(object, id); });
    FontSource(f, bindings);
    TextureAssets::Snapshot snapshot;
    CHECK(f.assets.Capture(snapshot) == TextureAssets::Result::Ok);
    auto ram = f.ram; ram.resize(0x02000000);
    const auto put = [&](uint32_t at, uint32_t value, unsigned width = 4) {
        TextureTestData::Put(ram, at, value, width);
    };
    constexpr uint32_t table = Policy::kTextureTable, rows = 0x1a0000;
    put(table, rows); put(table + 4, 3, 2); put(table + 6, 64, 2);
    put(table + 8, 64, 2); put(table + 0x10, 0); put(table + 0x14, Policy::kTableVtable);
    for (unsigned i = 0; i < 3; ++i) {
        put(rows + i * 8, F::object + (2 - i) * 0x100);
        put(rows + i * 8 + 4, pages[2 - i]);
    }
    const TextureBindings::Memory memory{ram.data(), ram.size()};
    std::string why;
    const auto preflight = [&](const TextureAssets::Snapshot& state, Policy::Services ready = {true, true}) {
        return Policy::PreflightSnapshot(state, memory, ready, &why);
    };
    CHECK(preflight(snapshot));
    CHECK(!preflight(snapshot, {false, true}));
    auto missing = snapshot; missing.assets.pop_back();
    std::vector<uint8_t> bytes;
    TextureAssets::Snapshot decoded;
    CHECK(TextureStateCodec::Encode(missing, bytes) && TextureStateCodec::Decode(bytes, decoded));
    CHECK(!preflight(decoded)); // valid payload cannot omit an incoming selected owner
    TextureAssets::Snapshot empty;
    CHECK(!preflight(empty)); // absent/missing-payload Reset would be incorrect
    for (const auto& pair : {std::pair<uint32_t, uint32_t>{table, 0},
            {table, 0x01fffffc}, {table + 0x14, 0}, {table + 0x10, 0x1b0000}, // a foreign queue pointer
            {rows + 4, pages[0]}, {rows, F::object}, {Policy::kTableVtable + 0x24, 0}}) {
        const auto before = TextureTestData::Word(ram.data() + pair.first);
        put(pair.first, pair.second);
        CHECK(!preflight(snapshot));
        put(pair.first, before);
    }
    put(table + 4, 2, 2);
    CHECK(!preflight(snapshot)); // orphan serialized owner
    put(table + 4, 65, 2);
    CHECK(!preflight(snapshot)); // count/capacity disagreement
    put(table + 4, 3, 2);
    put(Policy::kTableVtable + 0x20, 4, 2);
    CHECK(!preflight(snapshot)); // wrong this-adjustment
    put(Policy::kTableVtable + 0x20, 0, 2);
    // Source pointers may use a verified RAM segment alias; identity stays physical.
    put(rows, (F::object + 0x200) | 0xa0000000);
    CHECK(preflight(snapshot));
    // The title's static deletion queue (ConstructBase at boot) is the ONLY
    // non-null queue; an empty one changes nothing.
    put(table + 0x10, Policy::kTextureQueue);
    put(Policy::kTextureQueue, 0, 2); put(Policy::kTextureQueue + 2, 0, 2);
    CHECK(preflight(snapshot));
    // A released (queued) selected object is still allocated: it pairs with
    // its TXAS owner like a table row, wherever the ring's head is.
    put(table + 4, 2, 2);                       // page 0's row (F::object) left the table ...
    put(Policy::kTextureQueue, 0x2fe, 2); put(Policy::kTextureQueue + 2, 2, 2);
    put(Policy::kTextureQueue + 4 + 0x2fe * 4, F::object);   // ... into the queue (ring wraps)
    put(Policy::kTextureQueue + 4, 0);          // a null item is skipped (DeleteFirst)
    CHECK(preflight(snapshot));
    put(Policy::kTextureQueue + 4 + 0x2fe * 4, F::object + 0x300);   // a queued object TXAS lacks
    put(F::object + 0x300 + 4, pages[0]);
    CHECK(!preflight(snapshot) && why.find("absent from TXAS") != std::string::npos);
    put(Policy::kTextureQueue + 4 + 0x2fe * 4, 0);
    CHECK(!preflight(snapshot) && why.find("owner is absent from incoming TextureTable") != std::string::npos);
    put(Policy::kTextureQueue, 0x2ff, 2);       // malformed head
    CHECK(!preflight(snapshot) && why.find("deletion queue is malformed") != std::string::npos);
    put(Policy::kTextureQueue, 0, 2); put(Policy::kTextureQueue + 2, 0, 2);
    put(table + 4, 3, 2);
    put(table + 0x10, 0);
    // No selected table entry exists at this explicit constructed empty point.
    put(table, 0); put(table + 4, 0, 2); put(table + 6, 0, 2);
    CHECK(preflight(empty) && !preflight(snapshot));
}

// Textures at load (`textures`): every record converts; a converted texture a
// UI producer does not own is a counted DECLINE at the Sprite (the original
// draws it from the owned slot bind), never a fault; the snapshot preflight
// pairs TXAS with the ownership word (+0x60), not the UI selection.
void TexturesAtLoad() {
    F f;
    GraphicsRefusals ledger;
    Policy policy(f.assets, {f.ram.data(), f.ram.size()}, {false, true, true}, &ledger);
    TextureBindings bindings(f.assets, {f.ram.data(), f.ram.size()},
        [&](uint32_t object, uint32_t id) { return policy.Texture(object, id); });
    // The load policy takes every id (a world texture, Arial, an unknown id).
    CHECK(policy.Texture(F::object, 19) && policy.Texture(F::object, 0x19235bb6) && policy.Texture(F::object, 0xdeadbeef));
    CHECK(policy.DeclinedTextures().empty());
    CHECK(Policy::ConvertedId(0xdeadbeef, {false, true, true}) && !Policy::ConvertedId(0xdeadbeef, {false, true, false}));
    CHECK(!Policy::SelectedId(0xdeadbeef, {false, true, true}));   // the UI selection is unchanged
    // A converted world texture (id 19) at a Sprite: declined as Unselected, not refused.
    SpriteSource(f, 19);
    Load(f, bindings, F::object, 19);
    CHECK(!policy.Sprite(sprite));
    const auto declines = ledger.Of(GraphicsRefusals::Family::Sprites);
    CHECK(declines.total == 1 && declines.first.detail == uint32_t(Policy::Decline::Unselected) &&
          declines.first.producer == F::object);
    CHECK(policy.UnselectedSprites().size() == 1 && policy.UnselectedSprites()[0].texture == 19);
    // The same consumer WITHOUT `textures` is the fault it always was.
    Policy strict(f.assets, {f.ram.data(), f.ram.size()}, {false, true, false});
    CHECK(Refused([&] { strict.Sprite(sprite); }));
    // A converted UI picture stays the owned producer's; the title's slot word
    // (set by a model consumer's upload) does not change that.
    CHECK(f.assets.Reset() == TextureAssets::Result::Ok);
    SpriteSource(f, hud);
    Load(f, bindings, F::object, hud);
    CHECK(policy.Sprite(sprite));
    f.Put(F::object + 0x64, 0xbad570);
    CHECK(policy.Sprite(sprite));
    // Preflight: under `textures` a table object pairs with TXAS iff it carries
    // no DiskManager transfer; one the original loaded (handle >= 0) must be absent.
    TextureAssets::Snapshot snapshot;
    CHECK(f.assets.Capture(snapshot) == TextureAssets::Result::Ok && snapshot.assets.size() == 1);
    auto ram = f.ram; ram.resize(0x02000000);
    const auto put = [&](uint32_t at, uint32_t value, unsigned width = 4) { TextureTestData::Put(ram, at, value, width); };
    constexpr uint32_t table = Policy::kTextureTable, rows = 0x1a0000, world = F::object + 0x400;
    put(table, rows); put(table + 4, 2, 2); put(table + 6, 64, 2);
    put(table + 8, 64, 2); put(table + 0x10, 0); put(table + 0x14, Policy::kTableVtable);
    put(rows, world); put(rows + 4, 0x7fffffff);           // a world texture the original loaded
    put(world + 4, 0x7fffffff); put(world + 0x60, 12);     // its DiskManager transfer handle
    put(rows + 8, F::object); put(rows + 12, hud);
    const TextureBindings::Memory memory{ram.data(), ram.size()};
    std::string why;
    CHECK(Policy::PreflightSnapshot(snapshot, memory, {false, true, true}, &why));
    put(world + 0x60, 0xffffffff);                         // owned-looking but absent from TXAS
    CHECK(!Policy::PreflightSnapshot(snapshot, memory, {false, true, true}, &why) &&
          why.find("absent from TXAS") != std::string::npos);
    put(world + 0x60, 12);
    put(F::object + 0x60, 3);                              // the TXAS owner lost its ownership word
    CHECK(!Policy::PreflightSnapshot(snapshot, memory, {false, true, true}, &why));
    put(F::object + 0x60, 0xffffffff);
    CHECK(Policy::PreflightSnapshot(snapshot, memory, {false, true, true}, &why));
    // Without `textures` the UI selection decides as before (the world row is unselected).
    CHECK(Policy::PreflightSnapshot(snapshot, memory, {false, true, false}, &why));
}
}

int main() {
    SpriteSelection();
    WidenedSelection();
    FontSelection();
    ServiceFailure();
    CheckpointClosure();
    TexturesAtLoad();
    std::printf("TextureSelection: %d checks, %d failures; source ownership and no-fallback pins\n", checks, failures);
    return failures ? 1 : 0;
}
