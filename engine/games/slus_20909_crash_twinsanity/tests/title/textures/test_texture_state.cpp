#include <algorithm>
#include <cstdio>
#include <limits>

#include "engine/gs/format/swizzle.h"
#include "engine/gs/hle/texsrc/texture_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_bindings.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_state_codec.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_backend.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_data.h"

namespace {
using Assets = title::TextureAssets;
using Codec = title::TextureStateCodec;
using Queue = title::Graphics;
using Resources = title::GraphicsResources;
using Bindings = title::TextureBindings;
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)

bool Same(const Assets::Snapshot& a, const Assets::Snapshot& b) {
    std::vector<uint8_t> left, right;
    return Codec::Encode(a, left) && Codec::Encode(b, right) && left == right;
}
Queue::Hooks Hooks(Assets& assets) {
    Queue::Hooks hooks;
    hooks.upload = [&](const Queue::Upload& upload) { return assets.Upload(upload, true) == Assets::Result::Ok; };
    hooks.drain = [] { return true; };
    return hooks;
}
void ModelPixels(const std::vector<uint8_t>& original, const title::TextureCodec::Image& image) {
    // SAME original bytes through the current NEW writer/decoder, test-only.
    using namespace ps2r::gs;
    const auto* h = original.data() + 4;
    const auto* storage = original.data() + 132;
    std::vector<uint8_t> memory(Swizzle::kVramBytes);
    const auto word = TextureTestData::Word;
    Swizzle::LinearToVram(memory.data(), {Psm::CT32, 0, h[9] == 0 ? word(h + 0x2c) : 1},
        {0, 0, word(storage + 48), word(storage + 52)}, storage + 96, size_t(word(storage + 48)) * 4);
    TexReadView view;
    view.psm = h[9]; view.cpsm = 0; view.csm = 0; view.cbp = word(h + 0x48) * 256;
    for (size_t i = 0; i < image.levels.size(); ++i) {
        const auto& level = image.levels[i];
        std::vector<uint32_t> expected;
        TextureDecoder::DecodeLevel(memory.data(), view, word(h + 0x10 + i * 4) * 256,
            word(h + 0x2c + i * 4) * 64, level.width, level.height, nullptr, nullptr, &expected);
        bool equal = expected.size() * 4 == level.rgba.size();
        for (size_t n = 0; equal && n < expected.size(); ++n)
            equal = word(level.rgba.data() + n * 4) == expected[n];
        CHECK(equal);
    }
}
Assets::Snapshot State() {
    TextureTestBackend backend;
    Resources resources(backend);
    Queue queue;
    Assets assets(queue, resources, backend);
    Assets::View view;
    auto indexed = TextureTestData::Record();
    auto direct = TextureTestData::Record(false);
    CHECK(assets.Load(0x100000, 0x922ed81e, indexed.data(), indexed.size(), view) == Assets::Result::Ok);
    // BUG-010/082: same address/id, changed only a mip, then save the latest
    // immutable generation. The superseded generation must not reappear.
    indexed[132 + 96 + 16384] ^= 0xff;
    CHECK(assets.Load(0x100000, 0x922ed81e, indexed.data(), indexed.size(), view) == Assets::Result::Ok);
    CHECK(assets.Load(0x110000, 0x96432c5c, direct.data(), direct.size(), view) == Assets::Result::Ok);
    view = {};
    Assets::Snapshot snapshot;
    CHECK(assets.Capture(snapshot) == Assets::Result::Ok);
    CHECK(snapshot.assets.size() == 2 && snapshot.assets[0].token.generation == 2 &&
          snapshot.assets[1].token.generation == 3 && snapshot.next_generation == 4);
    ModelPixels(indexed, snapshot.assets[0].image);
    ModelPixels(direct, snapshot.assets[1].image);
    return snapshot;
}
void Roundtrip(const Assets::Snapshot& original) {
    std::vector<uint8_t> bytes;
    CHECK(Codec::Encode(original, bytes));
    Assets::Snapshot decoded;
    CHECK(Codec::Decode(bytes, decoded) && Same(decoded, original));
    TextureTestBackend backend;
    Resources resources(backend);
    Queue queue;
    Assets assets(queue, resources, backend);
    std::vector<Assets::Token> uploads;
    CHECK(assets.PreflightRestore(decoded) == Assets::Result::Ok);
    CHECK(assets.Restore(decoded, uploads) == Assets::Result::Ok && uploads.size() == 2);
    CHECK(backend.creates == 0 && backend.uploads == 0 && assets.Counts().loaded == 0);
    Assets::Snapshot recaptured;
    CHECK(assets.Capture(recaptured) == Assets::Result::Ok && Same(original, recaptured));
    CHECK(assets.PreflightRestore(decoded) == Assets::Result::OwnerNotEmpty);
    // Modify the decoded snapshot after publication: restored bytes are owned.
    decoded.assets[0].image.levels[1].rgba[0] ^= 0xff;
    CHECK(queue.BeginFrame(77) == Queue::Result::Ok);
    for (const auto token : uploads) CHECK(assets.Record(queue.End(24), token) == Assets::Result::Ok);
    CHECK(assets.Capture(recaptured) == Assets::Result::PendingWork && Same(original, recaptured));
    CHECK(queue.Submit(Hooks(assets)) == Queue::Result::Ok);
    // The mip chain is finished for every image with levels BEYOND the base (the
    // backend's `authored_levels` convention, texsrc's `chain`); a base-only image
    // has no chain to finish (fix 72dce6a8: the count including the base left the
    // chain incomplete and the owned mesh draws black).
    int chains = 0;
    for (const auto& saved : original.assets) chains += saved.image.levels.size() > 1;
    CHECK(backend.uploads == 2 && backend.mip_finishes == chains && queue.Counts().legacy_spans == 0);
    for (const auto& saved : original.assets) {
        Assets::View view;
        CHECK(assets.Find(saved.token, view) == Assets::Result::Ok && view.asset_id == saved.asset_id);
        ps2r::render::SurfaceId surface;
        CHECK(resources.Resolve(view.lease, &surface) == Resources::Result::Ok);
        CHECK(backend.At(surface).rgba == saved.image.levels[0].rgba);
        CHECK(view.image->palette == saved.image.palette);
        for (size_t i = 1; i < saved.image.levels.size(); ++i)
            CHECK(backend.mip_bytes[std::make_pair(surface.id, int(i))] == saved.image.levels[i].rgba);
    }
    auto record = TextureTestData::Record();
    Assets::View view;
    CHECK(assets.Load(0x100000, 0x922ed81e, record.data(), record.size(), view) == Assets::Result::Ok);
    CHECK(view.token.generation == original.next_generation);
    CHECK(assets.Find(original.assets[0].token, view) == Assets::Result::StaleGeneration);
}
void Refusals(const Assets::Snapshot& snapshot) {
    TextureTestBackend backend;
    Resources resources(backend);
    Queue queue;
    Assets assets(queue, resources, backend);
    auto record = TextureTestData::Record();
    Assets::View first, second;
    CHECK(assets.Load(0x100000, 1, record.data(), record.size(), first) == Assets::Result::Ok);
    auto out = snapshot;
    CHECK(assets.Capture(out) == Assets::Result::RetainedConsumer && Same(out, snapshot));
    CHECK(assets.Load(0x100000, 1, record.data(), record.size(), second) == Assets::Result::Ok);
    second = {};
    CHECK(assets.Capture(out) == Assets::Result::RetainedConsumer); // retired version still held
    first = {};
    CHECK(assets.Capture(out) == Assets::Result::Ok && out.assets.size() == 1);
    CHECK(queue.BeginFrame(1) == Queue::Result::Ok);
    CHECK(assets.Capture(out) == Assets::Result::PendingWork); // even an empty recording frame
    CHECK(assets.Record(queue.End(0), out.assets[0].token) == Assets::Result::Ok);
    backend.fail_mip = true;
    CHECK(queue.Submit(Hooks(assets)) == Queue::Result::DispatchFailed);
    CHECK(assets.Capture(out) == Assets::Result::PendingWork);
    CHECK(queue.Recover(Hooks(assets)) == Queue::Result::Ok);
    CHECK(assets.Capture(out) == Assets::Result::BackendFailed);
    std::vector<Assets::Token> uploads{{42, 99}};
    CHECK(assets.Restore(snapshot, uploads) == Assets::Result::OwnerNotEmpty);
    CHECK(uploads.size() == 1 && uploads[0].object == 42 && backend.uploads == 1);
    CHECK(assets.Reset() == Assets::Result::Ok);
    CHECK(assets.CollectAfterQuiescence() == Assets::Result::Ok);
    CHECK(assets.PreflightRestore(snapshot) == Assets::Result::OwnerNotEmpty);
}
void Rollback(const Assets::Snapshot& snapshot) {
    TextureTestBackend backend;
    Resources resources(backend, {8192, 16 * 1024 * 1024, 1});
    Queue queue;
    Assets assets(queue, resources, backend);
    std::vector<Assets::Token> output{{9, 7}};
    CHECK(assets.Restore(snapshot, output) == Assets::Result::QueueRejected);
    CHECK(output.size() == 1 && output[0].object == 9 && assets.Counts().current == 0);
    CHECK(assets.Counts().retained_bytes == 0 && backend.creates == 0);
    CHECK(assets.CollectAfterQuiescence() == Assets::Result::Ok && resources.Counts().live_bindings == 0);
    auto one = snapshot; one.assets.resize(1);
    CHECK(assets.Restore(one, output) == Assets::Result::Ok && output.size() == 1);
    CHECK(assets.Counts().current == 1 && output[0].generation == 2);
}
void Malformed(const Assets::Snapshot& original) {
    std::vector<uint8_t> bytes;
    CHECK(Codec::Encode(original, bytes));
    auto output = original;
    const auto reject = [&](const std::vector<uint8_t>& bad) {
        std::string error;
        CHECK(!Codec::Decode(bad, output, &error) && !error.empty() && Same(output, original));
    };
    for (size_t size = 0; size < 80; ++size) reject({bytes.begin(), bytes.begin() + size});
    for (size_t offset : {size_t{0}, size_t{4}, size_t{16}, size_t{20}, size_t{28},
                         size_t{36}, size_t{37}, size_t{38}, size_t{39}, size_t{40},
                         size_t{44 + 1024}, size_t{44 + 1024 + 4}, size_t{44 + 1024 + 8}}) {
        auto bad = bytes; bad[offset] = 0xff; reject(bad);
    }
    auto bad = bytes; bad.pop_back(); reject(bad);
    bad = bytes; bad.push_back(0); reject(bad);
    bad = bytes; TextureTestData::Put(bad, 8, 2, 8); reject(bad);
    const auto invalid = [&](Assets::Snapshot value) {
        auto preserved = bytes;
        CHECK(!Codec::Encode(value, preserved) && preserved == bytes);
    };
    auto value = original; value.version = 2; invalid(value);
    value = original; value.assets[1].token.object = value.assets[0].token.object; invalid(value);
    value = original; value.assets[1].token.generation = value.assets[0].token.generation; invalid(value);
    value = original; value.assets[0].token.generation = 0; invalid(value);
    value = original; std::swap(value.assets[0], value.assets[1]); invalid(value);
    value = original; value.assets[0].image.palette.pop_back(); invalid(value);
    value = original; value.assets[0].image.levels[1].width *= 2; invalid(value);
    value = original; value.assets[0].image.levels[1].rgba.pop_back(); invalid(value);
    CHECK(!Assets::ValidateSnapshot(original, {1, 128 * 1024 * 1024}));
    CHECK(!Assets::ValidateSnapshot(original, {4096, 1}));
    TextureTestBackend backend;
    Resources resources(backend);
    Queue queue;
    Assets limited(queue, resources, backend, {1, 128 * 1024 * 1024});
    CHECK(limited.PreflightRestore(original) == Assets::Result::InvalidSnapshot);
    value = original; value.next_generation = 0;
    value.assets.back().token.generation = std::numeric_limits<uint64_t>::max();
    CHECK(Codec::Encode(value, bytes) && Codec::Decode(bytes, output) && Same(value, output));
}
void MemoryAgreement(const Assets::Snapshot& snapshot) {
    std::vector<uint8_t> ram(0x02000000);
    for (size_t i = 0; i < snapshot.assets.size(); ++i) {
        const auto& asset = snapshot.assets[i];
        auto record = TextureTestData::Record(i == 0);
        const auto object = asset.token.object;
        TextureTestData::Put(ram, object + 4, asset.asset_id);
        std::copy_n(record.data(), 100, ram.data() + object + 8);
        TextureTestData::Put(ram, object + 0x60, 0xffffffff);
        TextureTestData::Put(ram, object + 0x64, 0);
    }
    const Bindings::Memory memory{ram.data(), ram.size()};
    const auto selected = [](uint32_t, uint32_t id) { return id == 0x922ed81e || id == 0x96432c5c; };
    CHECK(Bindings::ValidateSnapshotMemory(snapshot, memory, selected));
    CHECK(!Bindings::ValidateSnapshotMemory(snapshot, memory, [](uint32_t, uint32_t) { return false; }));
    CHECK(!Bindings::ValidateSnapshotMemory(snapshot, {ram.data(), ram.size() - 1}, selected));
    const auto object = snapshot.assets[0].token.object;
    for (const auto offset : {4, 0x10, 0x12, 0x14, 0x15, 0x16, 0x17, 0x19, 0x60}) {
        ram[object + offset] ^= 0xff;
        CHECK(!Bindings::ValidateSnapshotMemory(snapshot, memory, selected));
        ram[object + offset] ^= 0xff;
    }
    // The slot word is the title's (set by the owned slot upload): not identity.
    ram[object + 0x64] ^= 0xff;
    CHECK(Bindings::ValidateSnapshotMemory(snapshot, memory, selected));
    ram[object + 0x64] ^= 0xff;
    auto invalid = snapshot;
    invalid.assets[0].token.object = 0xfffffffcu;
    CHECK(!Bindings::ValidateSnapshotMemory(invalid, memory, selected));
    CHECK(Bindings::ValidateSnapshotMemory(snapshot, memory, selected));
}
}

int main() {
    const auto snapshot = State();
    Roundtrip(snapshot);
    Refusals(snapshot);
    Rollback(snapshot);
    Malformed(snapshot);
    MemoryAgreement(snapshot);
    std::printf("TextureState: %d checks, %d failures; SELF current NEW original-byte decode + restored upload\n",
                checks, failures);
    return failures ? 1 : 0;
}
