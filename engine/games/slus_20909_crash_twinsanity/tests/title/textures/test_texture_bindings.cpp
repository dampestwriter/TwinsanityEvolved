#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <vector>

#include "engine/gs/hle/texsrc/mip_sampler.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"

namespace {
using Bindings = title::TextureBindings;
using Assets = title::TextureAssets;
using Queue = title::Graphics;
using F = TextureBindingFixture;
using R = Bindings::Result;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)

void LoadBoundary() {
    F f;
    auto expected = f.ram;
    std::copy(f.record.begin(), f.record.begin() + 100, expected.begin() + F::object + 8);
    TextureTestData::Put(expected, F::object + 0x60, 0xffffffff);
    TextureTestData::Put(expected, F::object + 0x64, 0);
    TextureTestData::Put(expected, F::object + 0x6a, 0, 2);
    TextureTestData::Put(expected, F::stream + 12, F::data + f.record.size());
    Assets::View view;
    CHECK(f.bindings.Read(F::object, F::stream, view) == R::Ok);
    CHECK(f.ram == expected);
    CHECK(view.asset_id == 19 && view.token.object == F::object && view.image->levels.size() == 5);
    CHECK(f.assets.Counts().loaded == 1 && f.bindings.Counts().loaded == 1);
    CHECK(f.backend.creates == 0 && f.queue.Counts().legacy_spans == 0);
    f.Put(F::stream + 8, 0x80000000 | F::data);
    f.Put(F::stream + 12, 0x80000000 | F::data);
    // BUG-010 / TEXPOP: same address/id with changed mip bytes => new owner;
    // the earlier image remains immutable even after archive memory is reused.
    f.ram[F::data + 132 + 96 + 16384] ^= 0xff;
    Assets::View second;
    CHECK(f.bindings.Read(0xa0000000 | F::object, 0x80000000 | F::stream, second) == R::Ok);
    CHECK(second.token.generation != view.token.generation && second.token.object == view.token.object);
    CHECK(second.image->levels[0].rgba == view.image->levels[0].rgba);
    CHECK(second.image->levels[1].rgba != view.image->levels[1].rgba);
    CHECK(f.Word(F::stream + 12) == (0x80000000 | (F::data + f.record.size())));
    CHECK(f.assets.Find(view.token, second) == Assets::Result::StaleGeneration);
    CHECK(f.assets.Reset() == Assets::Result::Ok);
}
void Preflight() {
    F f;
    const auto pristine = f.ram;
    Assets::View output;
    output.asset_id = 123;
    const auto reject = [&](R result, uint32_t object = F::object, uint32_t stream = F::stream) {
        const auto before = f.ram;
        const auto actual = f.bindings.Read(object, stream, output);
        if (actual != result) std::fprintf(stderr, "preflight object=%x stream=%x: %d expected %d\n",
                                          object, stream, int(actual), int(result));
        CHECK(actual == result);
        CHECK(f.ram == before && f.assets.Counts().loaded == 0 && output.asset_id == 123);
        std::copy(pristine.begin(), pristine.end(), f.ram.begin());
    };
    reject(R::InvalidMemory, 0);
    reject(R::InvalidMemory, F::object + 1);
    reject(R::InvalidMemory, F::object | 0x40000000);
    reject(R::InvalidMemory, F::object, F::stream | 0x20000000);
    reject(R::InvalidMemory, 0x01ffffe0);
    reject(R::InvalidMemory, F::object, 0x10000000);
    f.Put(F::object + 0x60, 7); reject(R::ConflictingOwner);
    f.Put(F::stream, 0x306f00); reject(R::UnsupportedStream); // PAL vtable is not NTSC
    f.Put(Bindings::kMemoryStream + 0x10, 4, 2); reject(R::UnsupportedStream);
    f.Put(Bindings::kMemoryStream + 0xa8, 0xfffc, 2); reject(R::UnsupportedStream);
    f.Put(Bindings::kMemoryStream + 0x14, 0x2b6670); reject(R::UnsupportedStream);
    f.Put(Bindings::kMemoryStream + 0xac, 0x2b60e8); reject(R::UnsupportedStream);
    f.Put(F::stream + 8, 0x80000000 | F::data); reject(R::InvalidStream);
    f.Put(F::stream + 12, F::data - 1); reject(R::InvalidStream);
    f.Put(F::stream + 12, F::data + f.record.size() + 17); reject(R::InvalidStream);
    f.Put(F::stream + 16, 3); reject(R::InvalidStream);
    f.Put(F::stream + 16, f.record.size() - 1); reject(R::InvalidStream);
    f.Put(F::stream + 16, 0xffffffff); reject(R::InvalidStream);
    f.Put(F::data, 0xffffffff); reject(R::InvalidStream);
    f.Put(F::data, 0); reject(R::InvalidStream);
    f.ram[F::data + 13] = 0x14; reject(R::UnsupportedAsset); // BUG-064
    f.ram[F::data + 132] ^= 0xff; reject(R::UnsupportedAsset);
    f.ram[F::data + 12] = 0; reject(R::InvalidAsset);
    f.selected = false;
    f.Put(F::stream, 0);
    reject(R::NotSelected);
}
void OverlapAndLimits() {
    F f;
    Assets::View view;
    // Align a valid object inside the record, beyond its envelope.
    const uint32_t inside = F::data + 257;
    std::copy(f.ram.begin() + F::object, f.ram.begin() + F::object + 0x6c, f.ram.begin() + inside);
    const auto before = f.ram;
    CHECK(f.bindings.Read(inside, F::stream, view) == R::InvalidStream && f.ram == before);
    f.Put(F::object, Bindings::kMemoryStream);
    f.Put(F::object + 8, F::data); f.Put(F::object + 12, F::data);
    f.Put(F::object + 16, f.record.size());
    const auto overlapping = f.ram;
    CHECK(f.bindings.Read(F::object, F::object, view) == R::InvalidStream && f.ram == overlapping);
    // Registration refusal must leave both guest writes and output untouched.
    Queue queue({65536, 64 * 1024 * 1024, 0});
    Assets assets(queue, f.resources, f.backend);
    Bindings binding(assets, {f.ram.data(), f.ram.size()}, [](uint32_t, uint32_t) { return true; });
    f.Put(F::object, 0x80000001); f.Put(F::object + 4, 19);
    const auto limited = f.ram;
    CHECK(binding.Read(F::object, F::stream, view) == R::ResourceFailure);
    CHECK(f.ram == limited && assets.Counts().loaded == 0);
}
// The slot word (+0x64) is the title's slot allocator's, set once a model
// consumer uploads through the owned slot bind (TextureUploads): no ownership
// claim rides on it -- Read, Destroy and CaptureMaterial accept it set.
void SlotWord() {
    F f;
    f.Put(F::object + 0x64, 0xbad570);
    Assets::View view;
    CHECK(f.bindings.Read(F::object, F::stream, view) == R::Ok && view.asset_id == 19);
    CHECK(f.Word(F::object + 0x64) == 0);   // the owned header copy starts with no slot
    f.Put(F::object + 0x64, 0xbad570);      // ... the title's allocator sets it at the first upload
    f.Shader(0x01);
    Bindings::Material material;
    CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::Ok && material.source == Bindings::Source::Asset);
    unsigned freed = 0;
    CHECK(f.bindings.Destroy(F::object, 1, [&](uint32_t) { ++freed; }) == R::Ok && freed == 1);
}
void Destruction() {
    F f;
    Assets::View view, probe;
    CHECK(f.bindings.Read(F::object, F::stream, view) == R::Ok);
    CHECK(f.queue.BeginFrame(1) == Queue::Result::Ok);
    CHECK(f.assets.Record(f.queue.End(0), view.token) == Assets::Result::Ok);
    CHECK(f.bindings.Destroy(F::object, 1, {}) == R::MissingDeallocator);
    CHECK(f.assets.Find(view.token, probe) == Assets::Result::Ok && f.bindings.Counts().destroyed == 0);
    // A conflicting source identity on an owned object drops the claim: the
    // original's destroy/free must not leave a stale owned generation behind.
    f.Put(F::object + 4, 20);
    CHECK(f.bindings.Destroy(F::object, 1, {}) == R::ConflictingOwner);
    CHECK(f.assets.Find(F::object, probe) == Assets::Result::MissingAsset);
    CHECK(f.bindings.Destroy(F::object, 1, {}) == R::NotOwned);
    f.Put(F::object + 4, 19);
    f.Put(F::stream + 12, F::data);
    CHECK(f.bindings.Read(F::object, F::stream, view) == R::Ok);
    CHECK(f.assets.Record(f.queue.End(0), view.token) == Assets::Result::Ok);
    unsigned frees = 0;
    CHECK(f.bindings.Destroy(0x80000000 | F::object, 3, [&](uint32_t object) {
        CHECK(object == (0x80000000 | F::object));
        CHECK(f.assets.Find(F::object, probe) == Assets::Result::MissingAsset);
        ++frees;
        std::fill(f.ram.begin() + F::object, f.ram.begin() + F::object + 0x6c, 0xdd);
    }) == R::Ok);
    CHECK(frees == 1 && f.bindings.Counts().destroyed == 1);
    Queue::Hooks hooks;
    hooks.upload = [&](const Queue::Upload& upload) { return f.assets.Upload(upload, false) == Assets::Result::Ok; };
    hooks.drain = [] { return true; };
    CHECK(f.queue.Submit(hooks) == Queue::Result::Ok);
    CHECK(f.backend.level_uploads == 8); // both recorded generations upload; the released one is no longer the owner
    CHECK(f.bindings.Destroy(F::object, 0, {}) == R::NotOwned);
    CHECK(f.assets.Reset() == Assets::Result::Ok);
}
void Installer() {
    F f;
    std::map<uint32_t, Bindings::Body> installed;
    unsigned legacy = 0, freed = 0;
    const Bindings::Capture capture = [&](uint32_t) {
        return [&](CpuContext&, uint32_t) { ++legacy; };
    };
    const Bindings::Installer install = [&](uint32_t address, Bindings::Body body) {
        installed[address] = std::move(body); return true;
    };
    CHECK(!f.bindings.Install(capture, install, {}));
    CHECK(installed.empty());
    struct Refusal { title::GraphicsRefusals::Reason reason; uint32_t producer, detail; };
    std::vector<Refusal> refusals;
    CHECK(f.bindings.Install(capture, install, [&](CpuContext&, uint32_t p) { CHECK(p == F::object); ++freed; },
                             [&](auto reason, uint32_t producer, uint32_t detail) {
                                 refusals.push_back({reason, producer, detail});
                             }));
    CHECK(installed.size() == 2);
    CpuContext c{};
    c.cpuRegs.GPR.r[4].UL[0] = F::object; c.cpuRegs.GPR.r[5].UL[0] = F::stream;
    c.cpuRegs.GPR.r[29].UD[0] = 0x1ffff00; c.cpuRegs.GPR.r[31].UD[0] = 0xabcdef;
    const auto original = c;
    f.selected = false;
    installed.at(Bindings::kRead)(c, Bindings::kRead);
    CHECK(legacy == 1 && f.assets.Counts().loaded == 0);
    f.selected = true;
    // Interior continuations refuse to the original continuation: counted,
    // attributed (detail 0x1000), nothing loaded.
    unsigned interiors = 0;
    for (uint32_t continuation : {0x1bc960, 0x1bc980, 0x1bc9a8, 0x1bc9b4, 0x1bc9cc, 0x1bc9d8, 0x1bc9e4, 0x1bca04}) {
        installed.at(Bindings::kRead)(c, continuation);
        CHECK(legacy == 2 + interiors++ && f.assets.Counts().loaded == 0);
        CHECK(refusals.back().detail == 0x1000 && refusals.back().reason == title::GraphicsRefusals::Reason::Ordering);
    }
    legacy = 1;
    refusals.clear();
    // A selected read this owner cannot take REFUSES: counted, attributed with
    // its Result, and the original runs -- never an abort, never a commit.
    f.Put(F::stream + 16, 3);
    installed.at(Bindings::kRead)(c, 0);
    CHECK(legacy == 2 && f.assets.Counts().loaded == 0);
    CHECK(f.bindings.Counts().refused == 9 && refusals.size() == 1);
    CHECK(refusals.size() == 1 && refusals[0].producer == Bindings::kRead &&
          refusals[0].detail == uint32_t(R::InvalidStream) &&
          refusals[0].reason == title::GraphicsRefusals::Reason::UnsupportedState);
    f.Put(F::stream + 16, f.record.size());
    installed.at(Bindings::kRead)(c, 0);
    CHECK(legacy == 2 && f.assets.Counts().loaded == 1);
    CHECK(c.cpuRegs.GPR.r[29].UD[0] == original.cpuRegs.GPR.r[29].UD[0]);
    CHECK(c.cpuRegs.GPR.r[31].UD[0] == original.cpuRegs.GPR.r[31].UD[0]);
    // The gate-4 abort shape: TextureDestroy/TextureRead with an object outside
    // readable RAM (null here) is InvalidMemory -> refused to the original.
    c.cpuRegs.GPR.r[4].UL[0] = 0;
    installed.at(Bindings::kDestroy)(c, Bindings::kDestroy);
    installed.at(Bindings::kRead)(c, 0);
    CHECK(legacy == 4 && freed == 0 && f.assets.Counts().current == 1);
    CHECK(f.bindings.Counts().refused == 11 && refusals.size() == 3);
    CHECK(refusals[1].producer == Bindings::kDestroy && refusals[1].detail == uint32_t(R::InvalidMemory));
    CHECK(refusals[2].producer == Bindings::kRead && refusals[2].detail == uint32_t(R::InvalidMemory));
    // A re-read into an object this owner converted (transfer word set by the
    // source) drops the owned generation before the original takes the object.
    c.cpuRegs.GPR.r[4].UL[0] = F::object;
    f.Put(F::object + 0x60, 7);
    installed.at(Bindings::kRead)(c, 0);
    Assets::View probe;
    CHECK(legacy == 5 && f.assets.Counts().current == 0 &&
          f.assets.Find(F::object, probe) == Assets::Result::MissingAsset);
    CHECK(refusals.size() == 4 && refusals[3].detail == uint32_t(R::ConflictingOwner) &&
          refusals[3].reason == title::GraphicsRefusals::Reason::Ordering);
    f.Put(F::object + 0x60, 0xffffffff);
    f.Put(F::stream + 12, F::data);
    installed.at(Bindings::kRead)(c, 0);
    CHECK(legacy == 5 && f.assets.Counts().current == 1);
    c.cpuRegs.GPR.r[5].UL[0] = 1;
    installed.at(Bindings::kDestroy)(c, Bindings::kDestroy);
    CHECK(freed == 1 && legacy == 5 && f.assets.Counts().current == 0);
    CHECK(f.bindings.Counts().failed == 0 && f.bindings.Counts().declined == 1 &&
          f.bindings.Counts().refused == 12);
    CHECK(Bindings::RefusalReason(R::ResourceFailure) == title::GraphicsRefusals::Reason::MissingResource);
}
void MaterialContracts() {
    F f;
    Assets::View view;
    CHECK(f.bindings.Read(F::object, F::stream, view) == R::Ok);
    Bindings::Material material;
    for (unsigned type : {0x01, 0x02, 0x04, 0x14, 0x15, 0x16, 0x17, 0x19, 0x1a, 0x1b, 0x1e, 0x1f, 0x20, 0x0d}) {
        for (int k : {-2048, -120, -69, -1, 0, 2047}) {
            f.Shader(type, k, 3, false);
            CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::Ok);
            CHECK(material.source == Bindings::Source::Asset && material.asset.token.generation == view.token.generation);
            CHECK(material.minify == Bindings::Filter::LinearMipNearest && material.max_level == 4);
            CHECK(material.magnify == (type == 0x0d ? Bindings::Filter::Linear : Bindings::Filter::Nearest));
            CHECK(material.wrap == (type == 0x14 || type == 0x0d ? Bindings::Wrap::Clamp : Bindings::Wrap::Repeat));
            // OLD BUG-084: SAME signed source value against current NEW's
            // actual sign correction and downstream chain capture.
            ps2r::gs::TexReadView old;
            old.psm = 0x13; old.tw = 7; old.th = 7; old.mmin = 4; old.mxl = 4; old.l = 3;
            old.k = ps2r::gs::MipSampler::SignExtendK(uint16_t(k));
            auto chain = ps2r::gs::MipSampler::Capture(old);
            CHECK(chain.levels == material.max_level && chain.k == material.lod_bias && chain.l == material.lod_shift);
            CHECK(material.lod_bias == k && material.stq && material.alpha);
        }
    }
    for (unsigned type : {0x00, 0x0a, 0x0c, 0x0e, 0x0f, 0x12, 0x13}) {
        for (bool linear : {false, true}) {
            f.Shader(type, -69, 0, linear);
            CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::Ok);
            CHECK(material.minify == (linear ? Bindings::Filter::Linear : Bindings::Filter::Nearest));
            CHECK(material.magnify == material.minify && material.max_level == 4);
            CHECK(material.stq == (type != 0));
            f.Shader(type, -69, 0, linear, false);
            CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::Ok && material.stq == (type == 0));
        }
    }
    for (unsigned type : {0x10, 0x11, 0x18}) {
        f.Shader(type, -69, 0, true, false, false);
        f.Put(F::shader + 0x14, 0xffffffff); // scene-copy never acquires an asset
        CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::Ok);
        CHECK(material.source == Bindings::Source::SceneCopy && !material.asset.lease && material.stq);
        CHECK(material.alpha == (type != 0x18) && material.wrap == Bindings::Wrap::Clamp && material.max_level == 0);
    }
    for (unsigned type : {0x05, 0x06, 0x07, 0x08, 0x09, 0x0b, 0x1c, 0x1d, 0x21}) {
        f.Shader(type);
        CHECK(f.bindings.CaptureMaterial(F::shader, material) ==
              (type == 0x1d || type == 0x21 ? R::UnsupportedMaterial : R::InheritedMaterial));
        CHECK(material.source == Bindings::Source::SceneCopy); // no partial output
    }
    f.Shader(0x03);
    CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::Ok && material.source == Bindings::Source::None);
    f.Shader(0x01, -69, 0, true, true, false);
    CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::Ok && material.source == Bindings::Source::None);
    for (int k : {-2049, 2048}) {
        f.Shader(0x01, k);
        CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::UnsupportedLod);
    }
    for (int l : {-1, 4}) {
        f.Shader(0x01, 0, l);
        CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::UnsupportedLod);
    }
    f.Shader(0x01);
    f.Put(F::shader + 0x6c, 0x2fa948); // type-A method cannot supply type-1 mip policy
    CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::UnsupportedMaterial);
    f.Shader(0x01);
    f.Put(0x2faec8 + 0x28, 4, 2);
    CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::UnsupportedMaterial);
    f.Put(0x2faec8 + 0x28, 0, 2);
    f.Put(F::shader + 0x60, 20);
    CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::ConflictingOwner);
    CHECK(f.assets.Reset() == Assets::Result::Ok);
    f.Put(F::shader + 0x60, 19);
    CHECK(f.bindings.CaptureMaterial(F::shader, material) == R::NotOwned);
    F direct(false);
    CHECK(direct.bindings.Read(F::object, F::stream, view) == R::Ok);
    direct.Shader(0x0d, 0, 0, false);
    CHECK(direct.bindings.CaptureMaterial(F::shader, material) == R::Ok);
    CHECK(material.magnify == Bindings::Filter::Linear && material.minify == Bindings::Filter::Nearest && material.max_level == 0);
    CHECK(direct.assets.Reset() == Assets::Result::Ok);
}
// Cutover flip 1 (hle.graphics_sprites): the 40 startup-icon TextureReads at
// frame 60 refused InvalidMemory (detail 3). Their Stream is a stack object,
// and the title's main thread runs on the scratchpad (TR entry.cpp:83,
// stack.h:5-8; gamecontroller.cpp:448 `MemoryStream stream` for the icons,
// font.cpp:131 for the fonts). The owner must read/advance that Stream
// through the scratchpad window; the texture object itself stays RAM-keyed.
void ScratchpadStream() {
    F f;
    const uint32_t offset = F::stack_stream & 0x3fff;
    std::copy(f.ram.begin() + F::stream, f.ram.begin() + F::stream + 0x14, f.scratchpad.begin() + offset);
    std::fill(f.ram.begin() + F::stream, f.ram.begin() + F::stream + 0x14, 0);
    auto expected_ram = f.ram;
    std::copy(f.record.begin(), f.record.begin() + 100, expected_ram.begin() + F::object + 8);
    TextureTestData::Put(expected_ram, F::object + 0x60, 0xffffffff);
    TextureTestData::Put(expected_ram, F::object + 0x64, 0);
    TextureTestData::Put(expected_ram, F::object + 0x6a, 0, 2);
    auto expected_scratchpad = f.scratchpad;
    TextureTestData::Put(expected_scratchpad, offset + 12, F::data + f.record.size());
    Assets::View view;
    CHECK(f.bindings.Read(F::object, F::stack_stream, view) == R::Ok);
    CHECK(f.ram == expected_ram && f.scratchpad == expected_scratchpad);
    CHECK(view.asset_id == 19 && view.token.object == F::object && f.bindings.Counts().loaded == 1);
    // The same stack Stream through the installed wrapper: no refusal, the
    // original never runs (the flip-1 census shape: loaded, refused 0).
    std::map<uint32_t, Bindings::Body> installed;
    unsigned legacy = 0, refusals = 0;
    CHECK(f.bindings.Install([&](uint32_t) { return [&](CpuContext&, uint32_t) { ++legacy; }; },
                             [&](uint32_t address, Bindings::Body body) { installed[address] = std::move(body); return true; },
                             [](CpuContext&, uint32_t) {}, [&](auto, uint32_t, uint32_t) { ++refusals; }));
    TextureTestData::Put(f.scratchpad, offset + 12, F::data);
    CpuContext c{};
    c.cpuRegs.GPR.r[4].UL[0] = F::object; c.cpuRegs.GPR.r[5].UL[0] = F::stack_stream;
    c.cpuRegs.GPR.r[29].UD[0] = F::stack_stream - 0x40;
    installed.at(Bindings::kRead)(c, 0);
    CHECK(legacy == 0 && refusals == 0 && f.bindings.Counts().loaded == 2 && f.bindings.Counts().refused == 0);
    CHECK(TextureTestData::Word(f.scratchpad.data() + offset + 12) == F::data + f.record.size());
    // Preflight stays total: a Stream past the window's end, an unaligned one,
    // and a texture OBJECT on the stack (never persistable) are InvalidMemory.
    const auto before_ram = f.ram, before_scratchpad = f.scratchpad;
    const auto reject = [&](uint32_t object, uint32_t stream) {
        CHECK(f.bindings.Read(object, stream, view) == R::InvalidMemory);
        CHECK(f.ram == before_ram && f.scratchpad == before_scratchpad);
    };
    reject(F::object, 0x70003ff0);
    reject(F::object, F::stack_stream + 2);
    reject(F::object, 0x70004000 | offset); // outside the 16 KB window
    std::copy(f.ram.begin() + F::object, f.ram.begin() + F::object + 0x6c, f.scratchpad.begin() + 0x100);
    const auto stack_object = f.scratchpad;
    CHECK(f.bindings.Read(0x70000100, F::stack_stream, view) == R::InvalidMemory);
    CHECK(f.bindings.Destroy(0x70000100, 0, {}) == R::InvalidMemory);
    CHECK(f.scratchpad == stack_object);
    // Control: an owner composed without the window (the pre-fix shape)
    // refuses the stack Stream exactly as flip 1 read it: InvalidMemory.
    Bindings windowless(f.assets, {f.ram.data(), f.ram.size()}, [](uint32_t, uint32_t) { return true; });
    CHECK(windowless.Read(F::object, F::stack_stream, view) == R::InvalidMemory);
    CHECK(f.assets.Reset() == Assets::Result::Ok);
}
}
int main() {
    LoadBoundary(); Preflight(); OverlapAndLimits(); SlotWord(); Destruction(); Installer(); MaterialContracts();
    ScratchpadStream();
    std::printf("TextureBindings: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
