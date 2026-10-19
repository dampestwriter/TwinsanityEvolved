// tests/title/textures/test_texture_pack.cpp -- A4 texture packs: the
// TextureContentHash (stable across decodes, pixel-sensitive), the pack
// directory (override replaces by hash, falls back when absent, a refused PNG
// never serves, prewarm fills the cache), TextureAssets::Load applying the pack
// (the authored identity kept, the GPU pixels and the registered extent the
// pack's, at ANOTHER resolution), the slot route serving the pack's surface
// under the title's DECLARED tuple (resolution-independent sampling: the
// surface is the pack's size, the match and uv_tex_w the declared 1<<tw), and
// the --dump-textures writer (one <hash>.png per hash, index, contact sheet
// with hex labels) whose files round-trip as a pack.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/image/png.h"
#include "engine/gs/hle/texsrc/owned_texture_slots.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_uploads.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_dump.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pack.h"
#include "tests/lib/fake_backend.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"

namespace {
using Assets = title::TextureAssets;
using Codec = title::TextureCodec;
using Pack = title::TexturePack;
using Dump = title::TextureDump;
using Png = ps2r::image::Png;
using F = TextureBindingFixture;
using Uploads = title::TextureUploads;
using Slots = ps2r::gs::OwnedTextureSlots;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)

std::string g_scratch;
std::string Fresh(const char* name) {
    const std::string dir = g_scratch + "/" + name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}
// A solid-colour RGBA8 image of any size.
std::vector<uint8_t> Solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) { rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a; }
    return rgba;
}
void WritePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    std::string error;
    CHECK(Png::EncodeFile(path, rgba.data(), w, h, &error));
}

void ContentHash() {
    Codec::Image a, b;
    const auto record = TextureTestData::Record(true);
    CHECK(Codec::Decode(record.data(), record.size(), a) == Codec::Result::Ok);
    CHECK(Codec::Decode(record.data(), record.size(), b) == Codec::Result::Ok);
    CHECK(a.content_hash != 0 && a.content_hash == b.content_hash);
    CHECK(Codec::ContentHash(a) == a.content_hash);
    // Sensitive to one pixel byte and to the level dimensions; blind to the
    // object / asset id / palette bytes the levels already bake in.
    b.levels[0].rgba[5] ^= 1;
    CHECK(Codec::ContentHash(b) != a.content_hash);
    b = a;
    b.levels[0].width *= 2;
    CHECK(Codec::ContentHash(b) != a.content_hash);
    b = a;
    b.palette.clear();
    CHECK(Codec::ContentHash(b) == a.content_hash);
    // The RGBA8 record hashes differently from the indexed one (different pixels).
    Codec::Image direct;
    const auto rgba = TextureTestData::Record(false);
    CHECK(Codec::Decode(rgba.data(), rgba.size(), direct) == Codec::Result::Ok);
    CHECK(direct.content_hash != a.content_hash);
    CHECK(Pack::HexOf(0x0123456789abcdefull) == "0123456789abcdef");
    // ApplyReplacement: level 0 the replacement, the chain box-halved to the
    // authored level count; the authored levels untouched.
    Codec::Image replaced = a;
    const uint32_t w = a.levels[0].width * 4, h = a.levels[0].height * 4;
    CHECK(Codec::ApplyReplacement(replaced, w, h, Solid(w, h, 200, 40, 60)));
    CHECK(replaced.pack_levels.size() == a.levels.size());
    CHECK(replaced.pack_levels[0].width == w && replaced.pack_levels[0].height == h);
    CHECK(replaced.pack_levels.back().width == std::max(1u, w >> (a.levels.size() - 1)));
    CHECK(replaced.pack_levels[1].rgba[0] == 200 && replaced.pack_levels[1].rgba[1] == 40);
    CHECK(replaced.levels.size() == a.levels.size() && replaced.levels[0].rgba == a.levels[0].rgba &&
          Codec::ContentHash(replaced) == a.content_hash);
    CHECK(&replaced.Pixels() == &replaced.pack_levels && &a.Pixels() == &a.levels);
    CHECK(!Codec::ApplyReplacement(replaced, w, h, Solid(w, h - 1, 0, 0, 0)));   // byte count disagrees
    Codec::Image empty;
    CHECK(!Codec::ApplyReplacement(empty, 1, 1, Solid(1, 1, 0, 0, 0)));
}

void PackDirectory() {
    Codec::Image image;
    const auto record = TextureTestData::Record(true);
    CHECK(Codec::Decode(record.data(), record.size(), image) == Codec::Result::Ok);
    const std::string dir = Fresh("pack_dir");
    Pack none;
    std::string error;
    CHECK(!none.Open(dir + "/missing", &error) && !none.Active() && error.find("does not exist") != std::string::npos);
    CHECK(!none.Open(dir, &error) && !none.Active() && error.find("no <hash>.png") != std::string::npos);
    // The hash's PNG at twice the resolution, a stranger, a non-hash file, a broken PNG.
    const uint32_t w = image.levels[0].width * 2, h = image.levels[0].height * 2;
    WritePng(dir + "/" + Pack::HexOf(image.content_hash) + ".png", w, h, Solid(w, h, 10, 20, 30));
    WritePng(dir + "/" + Pack::HexOf(image.content_hash ^ 1) + ".png", 4, 4, Solid(4, 4, 1, 2, 3));
    WritePng(dir + "/readme.png", 2, 2, Solid(2, 2, 0, 0, 0));
    { std::FILE* f = std::fopen((dir + "/" + Pack::HexOf(0xdeadbeef) + ".png").c_str(), "wb"); std::fputs("nope", f); std::fclose(f); }
    Pack pack;
    CHECK(pack.Open(dir, &error) && pack.Active() && pack.Counts().files == 3);
    CHECK(pack.PathOf(image.content_hash) == dir + "/" + Pack::HexOf(image.content_hash) + ".png");
    CHECK(pack.PathOf(0x1234).empty());
    const auto hit = pack.Lookup(image.content_hash);
    CHECK(hit && hit->width == w && hit->height == h && hit->rgba[0] == 10 && hit->rgba[2] == 30);
    CHECK(pack.Lookup(image.content_hash).get() == hit.get());   // cached
    CHECK(!pack.Lookup(image.content_hash ^ 2));                  // absent: falls back
    CHECK(!pack.Lookup(0xdeadbeef));                              // refused PNG: never served
    CHECK(!pack.Lookup(0xdeadbeef));
    auto counts = pack.Counts();
    CHECK(counts.lookups == 5 && counts.hits == 2 && counts.misses == 3);
    CHECK(counts.decoded == 1 && counts.decode_failures == 1);
    // Prewarm decodes the rest; the refused one stays refused.
    pack.Prewarm();
    pack.Join();
    counts = pack.Counts();
    CHECK(counts.prewarmed == 1 && counts.decoded == 2 && counts.decode_failures == 1);
    CHECK(counts.cached_bytes == uint64_t(w) * h * 4 + 4 * 4 * 4);
    // A budget below the first file: served but not retained.
    Pack tight;
    CHECK(tight.Open(dir));
    tight.Prewarm(1);
    tight.Join();
    CHECK(tight.Counts().cached_bytes == 0 && tight.Counts().uncached == tight.Counts().decoded);
    CHECK(tight.Lookup(image.content_hash) && tight.Counts().uncached > 0);
}

// TextureAssets::Load with and without a pack: identity vs GPU pixels.
void LoadWithPack() {
    const std::string dir = Fresh("pack_load");
    F probe;
    Assets::View probe_view;
    CHECK(probe.bindings.Read(F::object, F::stream, probe_view) == title::TextureBindings::Result::Ok);
    const uint64_t hash = probe_view.image->content_hash;
    const uint32_t declared_w = probe_view.image->levels[0].width, declared_h = probe_view.image->levels[0].height;
    const uint32_t w = declared_w * 2, h = declared_h * 2;   // the pack at twice the resolution
    WritePng(dir + "/" + Pack::HexOf(hash) + ".png", w, h, Solid(w, h, 250, 10, 10));
    Pack pack;
    CHECK(pack.Open(dir));
    {
        F f;
        f.assets.SetPack(&pack);
        Assets::View view;
        CHECK(f.bindings.Read(F::object, F::stream, view) == title::TextureBindings::Result::Ok);
        CHECK(view.image->content_hash == hash);
        CHECK(view.image->levels[0].width == declared_w && view.image->levels[0].height == declared_h);
        CHECK(view.image->pack_levels.size() == view.image->levels.size());
        CHECK(view.image->Pixels()[0].width == w && view.image->Pixels()[0].height == h);
        CHECK(view.image->Pixels()[0].rgba[0] == 250);
        CHECK(f.assets.PackCounts().replaced == 1);
        // The registered allocation is the pack's extent; the upload path takes the pack's pixels.
        {
            ps2r::render::SurfaceId surface;
            const auto& base = view.image->Pixels()[0];
            const title::Graphics::Upload upload{view.lease, {0, 0, int32_t(base.width), int32_t(base.height)},
                                                 size_t(base.width) * 4, base.rgba, 1};
            CHECK(f.assets.Upload(upload, true) == Assets::Result::Ok);
            CHECK(f.resources.Resolve(view.lease, &surface) == title::GraphicsResources::Result::Ok);
            const auto desc = f.backend.DescribeSurface(surface);
            CHECK(desc.width == w && desc.height == h);
            // The authored pixels are refused as an upload (not the registered extent).
            const auto& authored = view.image->levels[0];
            const title::Graphics::Upload wrong{view.lease, {0, 0, int32_t(authored.width), int32_t(authored.height)},
                                                size_t(authored.width) * 4, authored.rgba, 1};
            CHECK(f.assets.Upload(wrong, true) == Assets::Result::InvalidUpload);
        }
        // The snapshot carries the AUTHORED image only (the pack is a run-time overlay).
        Assets::Snapshot snapshot;
        std::string error;
        view = Assets::View();
        const auto captured = f.assets.Capture(snapshot, &error);
        if (captured != Assets::Result::Ok) std::fprintf(stderr, "capture: %s\n", error.c_str());
        CHECK(captured == Assets::Result::Ok);
        CHECK(snapshot.assets.size() == 1 && snapshot.assets[0].image.levels[0].width == declared_w &&
              snapshot.assets[0].image.pack_levels.empty());
    }
    {
        // No pack: the converted texture exactly (the identity arm).
        F f;
        Assets::View view;
        CHECK(f.bindings.Read(F::object, F::stream, view) == title::TextureBindings::Result::Ok);
        CHECK(view.image->pack_levels.empty() && &view.image->Pixels() == &view.image->levels);
        CHECK(f.assets.PackCounts().replaced == 0);
    }
    {
        // A pack without this hash: falls back to the converted texture.
        const std::string other = Fresh("pack_other");
        WritePng(other + "/" + Pack::HexOf(hash ^ 0x55) + ".png", 8, 8, Solid(8, 8, 1, 1, 1));
        Pack absent;
        CHECK(absent.Open(other));
        F f;
        f.assets.SetPack(&absent);
        Assets::View view;
        CHECK(f.bindings.Read(F::object, F::stream, view) == title::TextureBindings::Result::Ok);
        CHECK(view.image->pack_levels.empty() && f.assets.PackCounts().replaced == 0);
        CHECK(absent.Counts().lookups == 1 && absent.Counts().misses == 1);
    }
}

// The slot route: the bind carries the DECLARED extent for the match and the
// pack's extent for the surface; the resolver's uv_tex_w stays 1<<tw.
void SlotServeAtAnotherResolution() {
    const std::string dir = Fresh("pack_slot");
    F probe;
    Assets::View probe_view;
    CHECK(probe.bindings.Read(F::object, F::stream, probe_view) == title::TextureBindings::Result::Ok);
    const auto& declared = probe_view.image->levels[0];
    const uint32_t w = declared.width * 3, h = declared.height * 3;   // not even a power of two
    WritePng(dir + "/" + Pack::HexOf(probe_view.image->content_hash) + ".png", w, h, Solid(w, h, 7, 8, 9));
    Pack pack;
    CHECK(pack.Open(dir));
    F f;
    f.assets.SetPack(&pack);
    Assets::View view;
    CHECK(f.bindings.Read(F::object, F::stream, view) == title::TextureBindings::Result::Ok);
    Slots::Texture t;
    CHECK(Uploads::Describe(f.ram.data() + F::object, *view.image, view.token.generation, t));
    CHECK(t.rgba.size() == view.image->levels.size());
    CHECK(t.rgba[0].w == declared.width && t.rgba[0].h == declared.height);
    CHECK(t.rgba[0].pw == w && t.rgba[0].ph == h && t.rgba[0].rgba == view.image->pack_levels[0].rgba.data());
    CHECK(t.rgba[1].pw == std::max(1u, w / 2) && t.rgba[1].rgba == view.image->pack_levels[1].rgba.data());
    ps2r::test::FakeBackend be;
    Slots slots(be);
    const uint32_t base = 0x300000;
    const uint8_t* hd = f.ram.data() + F::object + Uploads::kTextureInObject;
    t.pin = view.image;
    slots.Bind(base, std::move(t));
    ps2r::gs::TexReadView v;
    v.tbp = base; v.tbw = (unsigned(hd[0x2c]) | unsigned(hd[0x2d]) << 8) * 64;
    v.psm = 0x13; v.tw = hd[4]; v.th = hd[6];
    v.cbp = base + TextureTestData::Word(hd + 0x48) * 256; v.cpsm = 0; v.csm = 0; v.csa = 0;
    ps2r::gs::TextureSource out;
    CHECK(slots.Serve(v, &out));   // the declared tuple matches although the pixels are 3x
    CHECK(out.owner.kind == ps2r::gs::TextureSource::OwnerKind::Decoded);
    const auto desc = be.DescribeSurface(out.owner.surface);
    CHECK(desc.width == w && desc.height == h);   // the surface is the pack's extent
    CHECK(be.At(out.owner.surface).rgba[0] == 7 && be.At(out.owner.surface).rgba[2] == 9);
    CHECK(slots.Stats().serves == 1 && slots.Stats().misses == 0);
    // Without a pack the same bind builds the declared extent.
    F plain;
    Assets::View plain_view;
    CHECK(plain.bindings.Read(F::object, F::stream, plain_view) == title::TextureBindings::Result::Ok);
    Slots::Texture p;
    CHECK(Uploads::Describe(plain.ram.data() + F::object, *plain_view.image, 9, p));
    CHECK(p.rgba[0].pw == declared.width && p.rgba[0].ph == declared.height);
    ps2r::test::FakeBackend be2;
    Slots slots2(be2);
    p.pin = plain_view.image;
    slots2.Bind(base, std::move(p));
    ps2r::gs::TextureSource out2;
    CHECK(slots2.Serve(v, &out2));
    const auto desc2 = be2.DescribeSurface(out2.owner.surface);
    CHECK(desc2.width == declared.width && desc2.height == declared.height);
}

void DumpAndRoundTrip() {
    const std::string dir = Fresh("dump");
    Dump dump;
    std::string error;
    CHECK(!dump.Open("", &error) && !dump.Active());
    CHECK(dump.Open(dir + "/out", &error) && dump.Active());
    Codec::Image indexed, direct;
    const auto a = TextureTestData::Record(true), b = TextureTestData::Record(false);
    CHECK(Codec::Decode(a.data(), a.size(), indexed) == Codec::Result::Ok);
    CHECK(Codec::Decode(b.data(), b.size(), direct) == Codec::Result::Ok);
    dump.Record(indexed, 0x11);
    dump.Record(indexed, 0x12);   // the same hash again: counted, not rewritten
    dump.Record(direct, 0x13);
    dump.Finish();
    dump.Finish();
    const auto counts = dump.Counts();
    CHECK(counts.records == 3 && counts.written == 2 && counts.failures == 0);
    const std::string file = dir + "/out/" + Pack::HexOf(indexed.content_hash) + ".png";
    CHECK(std::filesystem::exists(file));
    CHECK(std::filesystem::exists(dir + "/out/" + Pack::HexOf(direct.content_hash) + ".png"));
    CHECK(std::filesystem::exists(dir + "/out/index.tsv"));
    Png::Image sheet;
    CHECK(Png::DecodeFile(dir + "/out/contact_sheet.png", sheet, &error));
    CHECK(sheet.width == 2 * Dump::kTile && sheet.height == Dump::kTile + Dump::kLabel);
    // The dumped PNG is the authored level 0 exactly.
    Png::Image back;
    CHECK(Png::DecodeFile(file, back, &error));
    CHECK(back.width == indexed.levels[0].width && back.height == indexed.levels[0].height &&
          back.rgba == indexed.levels[0].rgba);
    // ... and the dump directory IS a pack: every dumped texture replaces itself.
    Pack pack;
    CHECK(pack.Open(dir + "/out"));
    const auto hit = pack.Lookup(indexed.content_hash);
    CHECK(hit && hit->rgba == indexed.levels[0].rgba);
    CHECK(pack.Lookup(direct.content_hash));
    // The contact sheet: thumbnails in rows of 16 with hex labels in the 3x5 glyphs.
    std::vector<std::vector<uint8_t>> thumbs{Solid(8, 8, 255, 0, 0), Solid(16, 4, 0, 255, 0)};
    std::vector<Dump::Thumbnail> list{{0x0123456789abcdefull, 8, 8, &thumbs[0]}, {0xfedcba9876543210ull, 16, 4, &thumbs[1]}};
    uint32_t sw = 0, sh = 0;
    std::vector<uint8_t> rgba;
    Dump::ContactSheet(list, sw, sh, rgba);
    CHECK(sw == 2 * Dump::kTile && sh == Dump::kTile + Dump::kLabel);
    CHECK(rgba[(size_t(10) * sw + 10) * 4] == 255 && rgba[(size_t(10) * sw + 10) * 4 + 1] == 0);   // red tile
    // The second tile is letterboxed (16x4 -> 64x16 centred): row 10 is black, row 32 green.
    CHECK(rgba[(size_t(10) * sw + Dump::kTile + 10) * 4 + 1] == 0);
    CHECK(rgba[(size_t(32) * sw + Dump::kTile + 10) * 4 + 1] == 255);
    // Label row 1 of tile 0: glyph '0' top row is 111 -> pixels 0,1,2 white; the gap column 3 black.
    const size_t label_y = Dump::kTile + 1;
    CHECK(rgba[(label_y * sw + 0) * 4] == 255 && rgba[(label_y * sw + 2) * 4] == 255 && rgba[(label_y * sw + 3) * 4] == 0);
    // glyph '1' (digit index 1 at x 4..6): top row 010 -> only x=5 white.
    CHECK(rgba[(label_y * sw + 4) * 4] == 0 && rgba[(label_y * sw + 5) * 4] == 255 && rgba[(label_y * sw + 6) * 4] == 0);
    CHECK(Dump::Glyph(0xf)[4] == 0b100 && Dump::Glyph(0xe)[4] == 0b111);
    uint32_t ew = 0, eh = 0;
    Dump::ContactSheet({}, ew, eh, rgba);
    CHECK(ew == Dump::kTile && eh == Dump::kTile + Dump::kLabel);
}
}  // namespace

int main(int argc, char** argv) {
    g_scratch = argc > 1 ? argv[1] : std::filesystem::temp_directory_path().string();
    g_scratch += "/texture_pack_test";
    std::filesystem::create_directories(g_scratch);
    ContentHash();
    PackDirectory();
    LoadWithPack();
    SlotServeAtAnotherResolution();
    DumpAndRoundTrip();
    std::filesystem::remove_all(g_scratch);
    std::printf("TexturePack: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
