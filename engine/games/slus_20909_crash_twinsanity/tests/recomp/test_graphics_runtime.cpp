#include "games/slus_20909_crash_twinsanity/composition/graphics_runtime.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>

#include "games/slus_20909_crash_twinsanity/composition/graphics_import.h"
#include "engine/core/verify/memory_checkpointer.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_state_codec.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_state.h"
#include "tests/lib/fake_backend.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"

namespace {
using namespace ps2r;
using Runtime = app::GraphicsRuntime;
using Bindings = title::TextureBindings;
using Selection = title::TextureSelection;
using Codec = title::TextureStateCodec;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr uint32_t kHud = 0x046a8347, kRows = 0x1a0000;
class TestGraphicsRuntime {
public:
    test::FakeBackend backend;
    title::GraphicsSurfaces surfaces{backend};
    gs::VramImage vram;
    gs::VramLayout layout;
    gs::ScalePolicy scale{gs::ScaleSettings{1}, &layout};
    gs::RenderTargetCache cache{surfaces, vram, scale};
    gs::TargetRefresher refresher{surfaces, vram, cache};
    gs::TextureResolver resolver{surfaces, vram, cache, refresher, layout};
    gs::ScanoutComposer scanout{surfaces, cache};
    gs::Presenter presenter{scanout};
    gs::SequenceRecipe sequence{gs::SequenceRecipe::Constants{}};
    gs::DrawPipeline pipeline{gs::DrawPipeline::Deps{
        &surfaces, &vram, &cache, &refresher, &resolver, &scanout, &presenter, &layout, nullptr, &sequence}};
    title::GraphicsResources resources{surfaces};
    title::GraphicsRenderer renderer{surfaces, resources};
    title::GraphicsScene scene{surfaces, resources, renderer, pipeline};
    MainMemory ram;
    Scratchpad scratch;
    std::vector<uint8_t> arena = std::vector<uint8_t>(0x200000);   // the META arena section
    MemorySections memory{ram, scratch, arena.data(), arena.size()};
    StateRestorer restorer{memory};
    std::unique_ptr<app::GraphicsImport> import;
    std::map<uint32_t, Runtime::Body> dispatch;
    std::map<uint32_t, unsigned> model_calls;
    std::unique_ptr<Runtime> runtime;
    unsigned pre_restore = 0, original_captures = 0, frees = 0;
    explicit TestGraphicsRuntime(bool sky = false, bool legacy = false) {
        TextureBindingFixture initial;
        std::memcpy(ram.Data(), initial.ram.data(), initial.ram.size());
        Put(TextureBindingFixture::object + 4, kHud);
        Put(Selection::kTextureTable, kRows);
        Put(Selection::kTextureTable + 4, 0, 2);
        Put(Selection::kTextureTable + 6, 64, 2);
        Put(Selection::kTextureTable + 8, 64, 2);
        Put(Selection::kTextureTable + 0x10, 0);
        Put(Selection::kTextureTable + 0x14, Selection::kTableVtable);
        Runtime::Options options; options.sprites = true; options.sky = sky;
        app::GraphicsProducers::Output output;
        output.ui = [](auto, auto&, auto&) { return false; };
        output.sky = [](auto&, auto&) { return false; };
        output.movie = [](auto&) { return false; };
        output.record = [](auto, auto) { return false; };
        output.immediate = [](auto) { return false; };
        runtime = std::make_unique<Runtime>(ram, resources, renderer, scene, surfaces, options, output,
                                            Runtime::Window{scratch.Data(), scratch.Size()});
        CHECK(runtime->Install([this](uint32_t address) {
            ++original_captures;
            return Runtime::Body{[this, address](CpuContext&, uint32_t) { ++model_calls[address]; }};
        }, [this](auto address, auto body) {
            dispatch[address] = std::move(body); return true;
        }, [this](CpuContext&, uint32_t) { ++frees; }));
        if (legacy) {
            import = std::make_unique<app::GraphicsImport>(options);
            runtime->SetImport(import.get());
        }
        CHECK(runtime->RegisterState(restorer));
        restorer.RequireSectionOwners();
        restorer.SetPreRestore([this](std::string* reason) {
            if (!runtime->CanCheckpoint(reason)) return false;
            ++pre_restore;
            return true;
        });
    }
    void Put(uint32_t at, uint32_t value, unsigned size = 4) {
        std::memcpy(ram.Pointer(at, size), &value, size);
    }
    uint32_t Word(uint32_t at) const {
        uint32_t value;
        std::memcpy(&value, ram.Pointer(at, 4), 4);
        return value;
    }
    void Load() {
        CpuContext context{};
        context.cpuRegs.GPR.r[4].UL[0] = TextureBindingFixture::object;
        context.cpuRegs.GPR.r[5].UL[0] = TextureBindingFixture::stream;
        dispatch.at(Bindings::kRead)(context, Bindings::kRead);
        Put(kRows, TextureBindingFixture::object);
        Put(kRows + 4, kHud);
        Put(Selection::kTextureTable + 4, 1, 2);
    }
    static constexpr uint32_t sky_model = 0x21000, sky_counts = 0x22000;
    void LoadSky() {
        // One authored type-A strip, converted by the production load adapter.
        // The real archive's complete models are covered by SkyPassSelfOracle.
        std::vector<uint32_t> words{
            0, 0, 0x070000fa, 0,
            0x6c018000, 0x8003, 0x30024000, 0x512, 0,
            0x01000101, 0x64018001, 12, 0x8003, 0x01000104, 0x68038003,
            0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0,
            0x6c038004,
            0x3f800011, 0x3f800022, 0x3f800033, 0x8080,
            0x3f800044, 0x3f800055, 0x3f800066, 0x8080,
            0x3f800077, 0x3f800088, 0x3f800099, 0x80,
            0x14000000, 0x01000101};
        while (words.size() % 4) words.push_back(0);
        words[0] = 0x60000000 | uint32_t(words.size() / 4 - 1);
        std::memcpy(ram.Pointer(0x23000, words.size() * 4), words.data(), words.size() * 4);
        Put(sky_model + 4, 0x1234); Put(sky_model + 8, 1);
        Put(sky_model + 12, sky_counts); Put(sky_counts, 3);
        Put(sky_model + 16, sky_counts + 4); Put(sky_counts + 4, uint32_t(words.size() * 4));
        Put(sky_model + 24, sky_counts + 8); Put(sky_counts + 8, 33);
        Put(title::SkyAssets::kDiskManager + 0x58 + 33 * 4, 0x24000);
        Put(0x24000, 33); Put(0x24004, 0x23000); Put(0x24008, uint32_t(words.size() * 4));
        Put(0x2401c, 0x4001);
        CpuContext context{};
        context.cpuRegs.GPR.r[4].UL[0] = sky_model;
        dispatch.at(title::SkyAssets::kModelRead)(context, title::SkyAssets::kModelRead);
        CHECK(runtime->ProducerCounts().sky.models_loaded == 1);
    }
    std::vector<uint8_t> Save() { return restorer.SaveImage({}); }
};
void RestoreOwnership() {
    TestGraphicsRuntime f;
    CHECK(f.runtime->CanCheckpoint());
    f.Load();
    CpuContext retained{};
    f.dispatch.at(title::SkyPass::kDrawModel)(retained, title::SkyPass::kDrawModel);
    CHECK(f.runtime->ProducerCounts().sky.model.model_calls == 1);
    CHECK(f.runtime->TextureCounts().current == 1);
    CHECK(f.model_calls[Bindings::kRead] == 0);
    CHECK(f.backend.creates == 0 && f.backend.uploads == 0);
    const auto captured = f.original_captures;
    const auto state = f.Save();
    const auto before = f.Word(TextureBindingFixture::object + 4);
    std::string error;
    CHECK(f.restorer.RestoreImage(state.data(), state.size(), &error));
    CHECK(f.runtime->Restores() == 1 && f.pre_restore == 1);
    CHECK(f.runtime->TextureCounts().loaded == 1);
    CHECK(f.runtime->ProducerCounts().sky.model.model_calls == 1);
    CHECK(f.runtime->TextureCounts().current == 1);
    CHECK(f.original_captures == captured); // no recapture of installed service
    CHECK(f.backend.creates == 0 && f.backend.uploads == 0);
    CHECK(f.Word(TextureBindingFixture::object + 4) == before);
    CHECK(f.runtime->CanCheckpoint());
    CHECK(f.Save() == state); // exact converted bytes and generation allocator
    // Stable dispatch now reaches the replacement owner and its restored token.
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = TextureBindingFixture::object;
    context.cpuRegs.GPR.r[5].UL[0] = 0;
    f.dispatch.at(Bindings::kDestroy)(context, Bindings::kDestroy);
    CHECK(f.runtime->TextureCounts().current == 0 && f.frees == 0);
    CHECK(f.model_calls[Bindings::kDestroy] == 0);
}
void SkyComposition() {
    TestGraphicsRuntime f(true); f.Load(); f.LoadSky();
    CHECK(f.runtime->CanCheckpoint());
    const auto state = f.Save();
    const auto sections = f.restorer.Registry().Capture();
    const auto captures = f.original_captures;
    auto missing = sections;
    missing.erase(std::remove_if(missing.begin(), missing.end(),
        [](const auto& b) { return b.tag == title::SkyState::kTag; }), missing.end());
    auto omitted = sections;
    for (auto& b : omitted) if (b.tag == title::SkyState::kTag)
        CHECK(title::SkyState::Encode({}, b.payload));
    auto mismatch = sections;
    for (auto& b : mismatch) if (b.tag == savestate::TAG_RAM) b.payload[f.sky_model + 4] ^= 1;
    auto bad_count = sections;
    for (auto& b : bad_count) if (b.tag == savestate::TAG_RAM) b.payload[f.sky_counts] = 4;
    for (const auto* invalid : {&missing, &omitted, &mismatch, &bad_count}) {
        const auto bytes = savestate::SerializeStateFile({}, *invalid);
        std::string error;
        CHECK(!f.restorer.RestoreImage(bytes.data(), bytes.size(), &error));
        CHECK(!error.empty() && f.runtime->Restores() == 0 && f.pre_restore == 0);
        CHECK(f.Save() == state);
        CHECK(f.backend.creates == 0 && f.backend.uploads == 0);
    }
    std::string error;
    CHECK(f.restorer.RestoreImage(state.data(), state.size(), &error));
    CHECK(f.runtime->Restores() == 1 && f.original_captures == captures);
    CHECK(f.Save() == state && f.runtime->ProducerCounts().sky.models_loaded == 1);
    CHECK(f.runtime->TextureCounts().current == 1 && f.runtime->TextureCounts().loaded == 1);
    CHECK(f.backend.creates == 0 && f.backend.uploads == 0);
    // Destruction dispatch must forget the published restored owner.
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = f.sky_model;
    f.dispatch.at(title::SkyAssets::kModelDestroy)(context, title::SkyAssets::kModelDestroy);
    for (const auto& b : f.restorer.Registry().Capture()) if (b.tag == title::SkyState::kTag) {
        title::SkyAssets::Snapshot snapshot;
        CHECK(title::SkyState::Decode(b.payload, snapshot) && snapshot.models.empty());
    }
    TestGraphicsRuntime unselected;
    CHECK(!unselected.restorer.RestoreImage(state.data(), state.size(), &error));
    CHECK(unselected.pre_restore == 0 && unselected.runtime->Restores() == 0);
}
void InvalidImages() {
    TestGraphicsRuntime f; f.Load();
    auto sections = f.restorer.Registry().Capture();
    const auto initial = f.Save();
    const auto try_image = [&](const auto& blobs) {
        const auto bytes = savestate::SerializeStateFile({}, blobs);
        std::string error;
        CHECK(!f.restorer.RestoreImage(bytes.data(), bytes.size(), &error));
        CHECK(!error.empty());
        CHECK(f.pre_restore == 0 && f.runtime->Restores() == 0);
        CHECK(f.Save() == initial);
        CHECK(f.backend.creates == 0 && f.backend.uploads == 0);
    };
    auto missing = sections;
    missing.erase(std::remove_if(missing.begin(), missing.end(),
        [](const auto& b) { return b.tag == Codec::kTag; }), missing.end());
    try_image(missing); // OLD BUG-076: never turn missing ownership into reset
    auto incomplete = sections;
    for (auto& b : incomplete) if (b.tag == Codec::kTag) {
        title::TextureAssets::Snapshot empty;
        CHECK(Codec::Encode(empty, b.payload));
    }
    try_image(incomplete); // valid TXAS codec, selected RAM texture omitted
    auto wrong_ram = sections;
    for (auto& b : wrong_ram) if (b.tag == savestate::TAG_RAM)
        b.payload[TextureBindingFixture::object + 4] ^= 1;
    try_image(wrong_ram); // checks incoming RAM, not matching current live RAM
    auto bad_version = sections;
    for (auto& b : bad_version) if (b.tag == Codec::kTag) ++b.version;
    try_image(bad_version);
    // Successful preflight followed by another owner's refusal still publishes
    // nothing. The next attempt replaces this unpublished candidate safely.
    bool allow = false;
    f.restorer.AddImagePreflight([&](const auto&, std::string* reason) {
        if (!allow && reason) *reason = "other owner refused";
        return allow;
    });
    try_image(sections);
    allow = true;
    std::string error;
    CHECK(f.restorer.RestoreImage(initial.data(), initial.size(), &error));
    CHECK(f.runtime->Restores() == 1 && f.original_captures == f.dispatch.size());
}
// Cutover flip 1: the title's main thread runs on the scratchpad (TR
// entry.cpp:83), so the Stream a startup-icon TextureRead receives is a stack
// object at 0x7000xxxx. The composed owner reads it through the root's
// scratchpad window: loaded, no model call, no refusal; the stream cursor
// advances in the scratchpad; the owned object persists and restores.
void StackStream() {
    TestGraphicsRuntime f;
    const uint32_t stack_stream = TextureBindingFixture::stack_stream, offset = stack_stream & 0x3fff;
    std::memcpy(f.scratch.Pointer(offset, 0x14), f.ram.Pointer(TextureBindingFixture::stream, 0x14), 0x14);
    std::memset(f.ram.Pointer(TextureBindingFixture::stream, 0x14), 0, 0x14);
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = TextureBindingFixture::object;
    context.cpuRegs.GPR.r[5].UL[0] = stack_stream;
    context.cpuRegs.GPR.r[29].UL[0] = stack_stream - 0x30;
    f.dispatch.at(Bindings::kRead)(context, Bindings::kRead);
    CHECK(f.runtime->TextureCounts().loaded == 1 && f.runtime->TextureCounts().current == 1);
    CHECK(f.model_calls[Bindings::kRead] == 0);
    CHECK(f.runtime->Refusals().Of(title::GraphicsRefusals::Family::Textures).total == 0);
    uint32_t position;
    std::memcpy(&position, f.scratch.Pointer(offset + 12, 4), 4);
    CHECK(position == TextureBindingFixture::data + TextureBindingFixture().record.size());
    f.Put(kRows, TextureBindingFixture::object); f.Put(kRows + 4, kHud); f.Put(Selection::kTextureTable + 4, 1, 2);
    const auto state = f.Save();
    std::string error;
    CHECK(f.restorer.RestoreImage(state.data(), state.size(), &error));
    CHECK(f.runtime->Restores() == 1 && f.runtime->TextureCounts().current == 1);
}
// The explicit legacy import: a FROZEN image (RAM0/SPAD/META only -- the OLD
// schema) whose selected texture the ORIGINAL TextureRead loaded (header in
// the object, storage in the DiskManager block its transfer handle names)
// restores under the selected owner with TXAS derived from that memory and the
// owned commit words published into the restored RAM. Admission is the
// inventoried signature; refusals name what the image lacks.
void LegacyImport() {
    TestGraphicsRuntime f(false, true);
    constexpr uint32_t object = TextureBindingFixture::object, handle = 7, node = 0x24100, block = 0x25000;
    const TextureBindingFixture fixture;
    const auto& record = fixture.record;
    const uint32_t size = TextureTestData::Word(record.data());
    // The original's TextureRead: size at +8, the Texture header at +0xc, the
    // storage read into a disk block, the handle at +0x60 (TR resources.cpp).
    std::memcpy(f.ram.Pointer(object + 8, 4), record.data(), 4);
    std::memcpy(f.ram.Pointer(object + 0x0c, 0x60), record.data() + 4, 0x60);
    f.Put(object + 0x60, handle); f.Put(object + 0x64, 0); f.Put(object + 0x6a, 0, 2);
    std::memcpy(f.ram.Pointer(block, size - 0x80), record.data() + 4 + 0x80, size - 0x80);
    f.Put(title::TextureImport::kDiskManager + 0x58 + handle * 4, node);
    f.Put(node, handle); f.Put(node + 4, block); f.Put(node + 8, size - 0x80); f.Put(node + 0x1c, 0x4002);
    f.Put(kRows, object); f.Put(kRows + 4, kHud); f.Put(Selection::kTextureTable + 4, 1, 2);
    f.Put(Selection::kTextureTable + 0x10, Selection::kTextureQueue);   // ConstructBase's static queue
    const auto legacy_image = [&](std::vector<savestate::Blob>* out = nullptr) {
        std::vector<savestate::Blob> blobs;
        for (const auto tag : {savestate::TAG_RAM, savestate::TAG_SPAD, savestate::TAG_META}) {
            savestate::Blob blob; blob.tag = tag; blob.version = 1; blob.flags = savestate::kSectionRequired;
            if (tag == savestate::TAG_RAM) blob.payload.assign(f.ram.Data(), f.ram.Data() + f.ram.Size());
            else if (tag == savestate::TAG_SPAD) blob.payload.assign(f.scratch.Data(), f.scratch.Data() + f.scratch.Size());
            else blob.payload = f.arena;
            blobs.push_back(std::move(blob));
        }
        if (out) *out = blobs;
        return savestate::SerializeStateFile({}, blobs);
    };
    std::vector<savestate::Blob> blobs;
    const auto image = legacy_image(&blobs);
    const uint64_t signature = verify::MemoryCheckpointer::Hash({{f.ram.Data(), f.ram.Size()},
        {f.scratch.Data(), f.scratch.Size()}, {f.arena.data(), f.arena.size()}});
    std::string error;
    // Not approved: the frozen schema alone admits nothing.
    CHECK(!f.restorer.RestoreImage(image.data(), image.size(), &error));
    CHECK(error.find("not an inventoried frozen anchor") != std::string::npos);
    CHECK(f.runtime->Restores() == 0 && f.import->Count().refused == 1 && !f.import->Prepared());
    f.import->Approve(signature);
    // A disturbed object word differs from the inventoried signature.
    f.Put(object + 4, kHud ^ 1);
    const auto tampered = legacy_image();
    f.Put(object + 4, kHud);
    CHECK(!f.restorer.RestoreImage(tampered.data(), tampered.size(), &error));
    CHECK(error.find("differs from its inventoried frozen signature") != std::string::npos);
    // The live owner holds nothing before the import; the frozen image's
    // object carries the original's handle.
    CHECK(f.runtime->TextureCounts().current == 0 && f.Word(object + 0x60) == handle);
    // The live process at --load-state time: nothing loaded yet (an empty table).
    f.Put(Selection::kTextureTable + 4, 0, 2);
    std::memset(f.ram.Pointer(object, 0x6c), 0, 0x6c);
    const bool restored = f.restorer.RestoreImage(image.data(), image.size(), &error);
    if (!restored) std::fprintf(stderr, "legacy restore: %s\n", error.c_str());
    CHECK(restored);
    CHECK(f.runtime->Restores() == 1 && f.pre_restore == 1);
    CHECK(f.runtime->TextureCounts().current == 1 && f.runtime->TextureCounts().loaded == 0);
    CHECK(f.import->Count().admitted == 1 && f.import->Count().published == 1 && f.import->Count().assets == 1 &&
          f.import->Count().migrated_objects == 1 && !f.import->Prepared());
    CHECK(f.Word(object + 4) == kHud && f.Word(object + 0x60) == 0xffffffff && f.Word(object + 0x64) == 0);
    CHECK(f.Word(object + 8) == size);                        // the record identity stays the image's
    CHECK(f.Word(title::TextureImport::kDiskManager + 0x58 + handle * 4) == node);   // the disk pool as the image left it
    CHECK(f.backend.creates == 0 && f.backend.uploads == 0);  // no upload before the first ordered draw
    CHECK(f.runtime->CanCheckpoint());
    // The published owner is a complete NEW image: it captures GXSL/TXAS with
    // the derived asset and restores as such.
    const auto saved = f.Save();
    bool txas = false, gxsl = false;
    savestate::FileMeta meta; std::vector<savestate::Blob> sections;
    CHECK(savestate::ParseStateFile(saved.data(), saved.size(), &meta, &sections, &error));
    for (const auto& b : sections) {
        if (b.tag == Codec::kTag) {
            title::TextureAssets::Snapshot snapshot;
            txas = Codec::Decode(b.payload, snapshot) && snapshot.assets.size() == 1 &&
                   snapshot.assets[0].asset_id == kHud && snapshot.assets[0].token.object == object &&
                   !snapshot.assets[0].image.levels.empty();
        }
        if (b.tag == 0x4c535847) gxsl = true;
    }
    CHECK(txas && gxsl);
    CHECK(f.restorer.RestoreImage(saved.data(), saved.size(), &error) && f.runtime->Restores() == 2);
    // The owner answers the object from here: the destroy is owned, no model call.
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = object;
    f.dispatch.at(Bindings::kDestroy)(context, Bindings::kDestroy);
    CHECK(f.runtime->TextureCounts().current == 0 && f.model_calls[Bindings::kDestroy] == 0);
    // A frozen image whose selected texture the original never loaded refuses
    // by name; nothing is fabricated and nothing changes.
    TestGraphicsRuntime g(false, true);
    std::memcpy(g.ram.Data(), blobs[0].payload.data(), blobs[0].payload.size());
    g.Put(object + 0x60, 0xffffffff);
    std::vector<savestate::Blob> absent = blobs;
    absent[0].payload.assign(g.ram.Data(), g.ram.Data() + g.ram.Size());
    g.import->Approve(verify::MemoryCheckpointer::Hash({{g.ram.Data(), g.ram.Size()},
        {g.scratch.Data(), g.scratch.Size()}, {g.arena.data(), g.arena.size()}}));
    const auto no_transfer = savestate::SerializeStateFile({}, absent);
    CHECK(!g.restorer.RestoreImage(no_transfer.data(), no_transfer.size(), &error));
    CHECK(error.find("no transfer chain") != std::string::npos && g.runtime->Restores() == 0);
    // A sky selection has no derivation: refused by name.
    TestGraphicsRuntime h(true, true);
    h.import->Approve(signature);
    std::memcpy(h.ram.Data(), blobs[0].payload.data(), blobs[0].payload.size());
    CHECK(!h.restorer.RestoreImage(image.data(), image.size(), &error));
    CHECK(error.find("sky converted assets") != std::string::npos && h.runtime->Restores() == 0);
    // Without an import attached every Graphics section stays required.
    TestGraphicsRuntime plain;
    CHECK(!plain.restorer.RestoreImage(image.data(), image.size(), &error));
    CHECK(error.find("Graphics selection") != std::string::npos && plain.runtime->Restores() == 0);
}
}  // namespace
int main() {
    RestoreOwnership();
    InvalidImages();
    SkyComposition();
    StackStream();
    LegacyImport();
    std::printf("GraphicsRuntime: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
