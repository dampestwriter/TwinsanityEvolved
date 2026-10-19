// tests/title/textures/test_texture_uploads.cpp -- TextureUploads: the owned
// WriteTextureUpload adapter over the binding fixture's converted record. The
// empty tag written in the packet's place, the bind recorded under the tag's
// address and taken by the walk, the TEX0 tuple derived from the object header
// (level offsets / widths, CLUT, footprint) exactly as WriteTexturePairs forms
// it, the declines (unconverted object) and refusals (conflicting owner, bad
// slot/packet), the pending window, the installed body's registers.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_uploads.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"

namespace {
using Uploads = title::TextureUploads;
using Assets = title::TextureAssets;
using F = TextureBindingFixture;
using UR = Uploads::Result;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)

constexpr uint32_t kPacket = 0x200000, kSlot = 0x210000, kBase = 0x3f0000;

struct World {
    F f;
    Uploads uploads{f.assets, {f.ram.data(), f.ram.size(), f.scratchpad.data(), f.scratchpad.size()}};
    Assets::View view;
};
// The fixture's record converted, the slot and a packet area prepared.
void Prepare(World& w) {
    CHECK(w.f.bindings.Read(F::object, F::stream, w.view) == title::TextureBindings::Result::Ok);
    w.f.Put(kSlot, kBase / 256);           // TextureSlot::gsAddress (256-byte units)
    w.f.Put(kSlot + 4, 0); w.f.Put(kSlot + 8, 0); w.f.Put(kSlot + 12, 0);
    std::fill(w.f.ram.begin() + kPacket, w.f.ram.begin() + kPacket + 0x40, uint8_t(0xaa));
}
constexpr uint32_t TextureOf() { return F::object + Uploads::kTextureInObject; }

void WriteAndDescribe() {
    World w;
    Prepare(w);
    uint32_t end = 0;
    CHECK(w.uploads.Write(kPacket, TextureOf(), kSlot, &end) == UR::Ok);
    CHECK(end == kPacket + 16);
    // The empty CNT tag (QWC 0, VIF NOP NOP); the bytes past it untouched.
    CHECK(w.f.Word(kPacket) == Uploads::kEmptyCntTag && w.f.Word(kPacket + 4) == 0 &&
          w.f.Word(kPacket + 8) == 0 && w.f.Word(kPacket + 12) == 0);
    CHECK(w.f.ram[kPacket + 16] == 0xaa && w.f.ram[kPacket + 0x3f] == 0xaa);
    CHECK(w.uploads.Pending() == 1 && w.uploads.Counts().pending == 1);
    // The tuple from the header: the fixture's indexed 5-level record.
    Uploads::Upload up;
    CHECK(!w.uploads.TakeTag(kPacket + 16, false, up));
    CHECK(w.uploads.TakeTag(0x80000000u | kPacket, false, up));   // the walk hands the physical address
    CHECK(w.uploads.Pending() == 0 && w.uploads.Counts().taken == 1);
    CHECK(up.base == kBase && up.object == F::object);
    const auto& t = up.texture;
    const uint8_t* h = w.f.ram.data() + F::object + Uploads::kTextureInObject;
    const auto word = [&](size_t at) { return w.f.Word(F::object + Uploads::kTextureInObject + at); };
    CHECK(t.key == w.view.token.generation && t.psm == h[9] && t.psm == 0x13);
    CHECK(t.tw == (unsigned(h[4]) | unsigned(h[5]) << 8) && t.th == (unsigned(h[6]) | unsigned(h[7]) << 8));
    CHECK(t.levels == h[8] && t.levels == 5 && t.rgba.size() == 5);
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(t.level_offset[i] == word(0x10 + i * 4) * 256);
        CHECK(t.level_tbw[i] == (i == 0 ? word(0x2c) : std::max(1u, word(0x2c + i * 4))) * 64);
        CHECK(t.rgba[i].w == w.view.image->levels[i].width && t.rgba[i].h == w.view.image->levels[i].height);
        CHECK(t.rgba[i].rgba == w.view.image->levels[i].rgba.data());
    }
    CHECK(t.clut_offset == word(0x48) * 256);
    CHECK(t.footprint == w.f.Word(F::object + 8) - 0x60 - 0x20 - 96);
    CHECK(t.footprint == w.f.record.size() - 4 - 0x60 - 0x20 - 96);
    CHECK(t.pin.get() == w.view.image.get());
    // Describe alone agrees with the bind; another image's shape is refused.
    ps2r::gs::OwnedTextureSlots::Texture again;
    CHECK(Uploads::Describe(w.f.ram.data() + F::object, *w.view.image, 7, again) && again.key == 7 &&
          again.footprint == t.footprint && again.level_offset[4] == t.level_offset[4]);
    title::TextureCodec::Image other = *w.view.image;
    other.levels.pop_back();
    CHECK(!Uploads::Describe(w.f.ram.data() + F::object, other, 7, again));
    other = *w.view.image;
    other.format = title::TextureCodec::Format::Rgba8;
    CHECK(!Uploads::Describe(w.f.ram.data() + F::object, other, 7, again));
}

void DeclinesAndRefusals() {
    World w;
    Prepare(w);
    uint32_t end = 1;
    const auto before = w.f.ram;
    // An object this owner never converted: declined, nothing written.
    CHECK(w.uploads.Write(kPacket, kSlot + 0x100 + Uploads::kTextureInObject, kSlot, &end) == UR::NotOwned);
    CHECK(w.f.ram == before && end == 1 && w.uploads.Pending() == 0);
    // The converted object with a DiskManager transfer word: the original owns it.
    w.f.Put(F::object + 0x60, 5);
    CHECK(w.uploads.Write(kPacket, TextureOf(), kSlot, &end) == UR::ConflictingOwner);
    w.f.Put(F::object + 0x60, 0xffffffff);
    // Another asset id in the object: conflicting.
    w.f.Put(F::object + 4, 20);
    CHECK(w.uploads.Write(kPacket, TextureOf(), kSlot, &end) == UR::ConflictingOwner);
    w.f.Put(F::object + 4, 19);
    // Unreadable / misaligned slot or packet: invalid memory, nothing written.
    CHECK(w.uploads.Write(kPacket, TextureOf(), 0x7fffff0, &end) == UR::InvalidMemory);
    CHECK(w.uploads.Write(kPacket, TextureOf(), kSlot + 2, &end) == UR::InvalidMemory);
    CHECK(w.uploads.Write(kPacket + 8, TextureOf(), kSlot, &end) == UR::InvalidMemory);
    CHECK(w.uploads.Write(0x3ffffff0, TextureOf(), kSlot, &end) == UR::InvalidMemory);
    CHECK(w.uploads.Write(kPacket, 2, kSlot, &end) == UR::InvalidMemory);
    // A header that no longer describes the image (level count): refused.
    w.f.ram[F::object + Uploads::kTextureInObject + 8] = 3;
    CHECK(w.uploads.Write(kPacket, TextureOf(), kSlot, &end) == UR::InvalidHeader);
    w.f.ram[F::object + Uploads::kTextureInObject + 8] = 5;
    CHECK(w.f.ram == before && w.uploads.Pending() == 0 && end == 1);
    // A packet on the scratchpad is keyed by its scratchpad identity.
    std::fill(w.f.scratchpad.begin(), w.f.scratchpad.begin() + 0x40, uint8_t(0xaa));
    CHECK(w.uploads.Write(0x70000010, TextureOf(), kSlot, &end) == UR::Ok && end == 0x70000020);
    CHECK(w.f.scratchpad[0x10] == 0 && w.f.scratchpad[0x13] == 0x10 && w.f.scratchpad[0x14] == 0 && w.f.scratchpad[0x20] == 0xaa);
    Uploads::Upload up;
    CHECK(!w.uploads.TakeTag(0x10, false, up) && w.uploads.TakeTag(0x10, true, up) && up.base == kBase);
}

void PendingWindow() {
    World w;
    Prepare(w);
    uint32_t end = 0;
    w.uploads.SetFrame(10);
    CHECK(w.uploads.Write(kPacket, TextureOf(), kSlot, &end) == UR::Ok);
    CHECK(w.uploads.Write(kPacket + 0x100, TextureOf(), kSlot, &end) == UR::Ok);
    CHECK(w.uploads.Pending() == 2);
    // Re-recording a packet address while its bind is pending replaces it.
    CHECK(w.uploads.Write(kPacket, TextureOf(), kSlot, &end) == UR::Ok);
    CHECK(w.uploads.Pending() == 2 && w.uploads.Counts().overwritten == 1);
    // The window: a bind survives kPendingFrames - 1 Links, then is dropped.
    w.uploads.SetFrame(10 + Uploads::kPendingFrames - 1);
    CHECK(w.uploads.Pending() == 2 && w.uploads.Counts().discarded == 0);
    w.uploads.SetFrame(10 + Uploads::kPendingFrames);
    CHECK(w.uploads.Pending() == 0 && w.uploads.Counts().discarded == 2);
    w.uploads.SetFrame(3);   // a clock that does not advance changes nothing
    CHECK(w.uploads.Write(kPacket, TextureOf(), kSlot, &end) == UR::Ok && w.uploads.Pending() == 1);
    Uploads::Upload up;
    CHECK(!w.uploads.TakeTag(kPacket + 0x100, false, up) && w.uploads.TakeTag(kPacket, false, up));
}

void Installed() {
    World w;
    Prepare(w);
    std::map<uint32_t, Uploads::Body> installed;
    int legacy = 0;
    CHECK(!w.uploads.Install({}, [&](uint32_t, Uploads::Body) { return true; }));
    CHECK(w.uploads.Install([&](uint32_t address) -> Uploads::Body {
                                CHECK(address == Uploads::kWriteUpload);
                                return [&](CpuContext&, uint32_t) { ++legacy; };
                            },
                            [&](uint32_t address, Uploads::Body body) { installed[address] = std::move(body); return true; }));
    CHECK(installed.size() == 1 && installed.count(Uploads::kWriteUpload) == 1);
    CpuContext c{};
    c.cpuRegs.GPR.r[5].UL[0] = kPacket;
    c.cpuRegs.GPR.r[6].UL[0] = TextureOf();
    c.cpuRegs.GPR.r[7].UL[0] = kSlot;
    installed[Uploads::kWriteUpload](c, 0);
    CHECK(c.cpuRegs.GPR.r[2].UL[0] == kPacket + 16 && legacy == 0);
    CHECK(w.uploads.Counts().calls == 1 && w.uploads.Counts().owned == 1 && w.uploads.Pending() == 1);
    // An interior entry: the original continuation owns it.
    installed[Uploads::kWriteUpload](c, Uploads::kWriteUpload + 0x24);
    CHECK(legacy == 1 && w.uploads.Counts().calls == 2 && w.uploads.Counts().owned == 1);
    // An unconverted object: the original (declined); a conflicting one: refused.
    c.cpuRegs.GPR.r[6].UL[0] = kSlot + 0x200 + Uploads::kTextureInObject;
    installed[Uploads::kWriteUpload](c, Uploads::kWriteUpload);
    CHECK(legacy == 2 && w.uploads.Counts().declined == 1);
    c.cpuRegs.GPR.r[6].UL[0] = TextureOf();
    w.f.Put(F::object + 0x60, 1);
    installed[Uploads::kWriteUpload](c, 0);
    CHECK(legacy == 3 && w.uploads.Counts().refused == 1 && w.uploads.Counts().owned == 1);
}
}

int main() {
    WriteAndDescribe(); DeclinesAndRefusals(); PendingWindow(); Installed();
    std::printf("TextureUploads: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
