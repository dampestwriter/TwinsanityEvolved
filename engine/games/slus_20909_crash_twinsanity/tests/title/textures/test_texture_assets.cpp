#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "engine/gs/format/swizzle.h"
#include "engine/gs/hle/texsrc/texture_decoder.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_assets.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_backend.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_data.h"

namespace {
using Codec = title::TextureCodec;
using Assets = title::TextureAssets;
using Queue = title::Graphics;
using Resources = title::GraphicsResources;
using namespace ps2r::gs;
int checks = 0, failures = 0;
uint64_t oracle_pixels = 0, oracle_levels = 0, oracle_assets = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)
void Put(std::vector<uint8_t>& data, size_t at, uint64_t word, unsigned size = 4) {
    TextureTestData::Put(data, at, word, size);
}
uint32_t Word(const uint8_t* p) { return TextureTestData::Word(p); }
std::vector<uint8_t> Record(bool indexed = true) { return TextureTestData::Record(indexed); }
bool Same(const Codec::Image& a, const Codec::Image& b) {
    if (a.format != b.format || a.alpha != b.alpha || a.function != b.function ||
        a.palette != b.palette || a.levels.size() != b.levels.size()) return false;
    for (size_t i = 0; i < a.levels.size(); ++i)
        if (a.levels[i].width != b.levels[i].width || a.levels[i].height != b.levels[i].height ||
            a.levels[i].rgba != b.levels[i].rgba) return false;
    return true;
}
void SelfOracle(const std::vector<uint8_t>& record) {
    Codec::Image actual;
    std::string error;
    const auto result = Codec::Decode(record.data(), record.size(), actual, &error);
    if (result != Codec::Result::Ok) std::fprintf(stderr, "%s\n", error.c_str());
    CHECK(result == Codec::Result::Ok);
    if (result != Codec::Result::Ok) return;
    const auto* h = record.data() + 4;
    const auto* storage = record.data() + 132;
    // Identical original asset bytes through current NEW's storage writer and
    // actual texture decoder. This mirror belongs ONLY to the test oracle.
    std::vector<uint8_t> memory(Swizzle::kVramBytes);
    const uint32_t width = Word(storage + 48), height = Word(storage + 52);
    Swizzle::LinearToVram(memory.data(), {Psm::CT32, 0, h[9] == 0 ? Word(h + 0x2c) : 1},
                         {0, 0, width, height}, storage + 96, size_t(width) * 4);
    TexReadView view;
    view.psm = h[9]; view.cpsm = 0; view.csm = 0;
    view.cbp = Word(h + 0x48) * 256;
    for (size_t i = 0; i < actual.levels.size(); ++i) {
        const auto& level = actual.levels[i];
        std::vector<uint32_t> expected;
        TextureDecoder::DecodeLevel(memory.data(), view, Word(h + 0x10 + i * 4) * 256,
            Word(h + 0x2c + i * 4) * 64, level.width, level.height, nullptr, nullptr, &expected);
        bool equal = expected.size() * 4 == level.rgba.size();
        for (size_t n = 0; equal && n < expected.size(); ++n)
            equal = Word(level.rgba.data() + n * 4) == expected[n];
        CHECK(equal);
        oracle_pixels += expected.size(); ++oracle_levels;
    }
    ++oracle_assets;
}
void CodecValidation() {
    auto good = Record();
    Codec::Image sentinel;
    CHECK(Codec::Decode(good.data(), good.size(), sentinel) == Codec::Result::Ok);
    const auto reject = [&](const std::vector<uint8_t>& bytes) {
        auto out = sentinel;
        std::string error;
        CHECK(Codec::Decode(bytes.data(), bytes.size(), out, &error) != Codec::Result::Ok);
        CHECK(!error.empty() && Same(out, sentinel));
    };
    for (size_t size = 0; size < 228; ++size) {
        auto bad = good; bad.resize(size); reject(bad);
    }
    for (size_t offset : {size_t{8}, size_t{10}, size_t{12}, size_t{15}, size_t{17},
                         size_t{132}, size_t{148}, size_t{164}, size_t{172}, size_t{188}, size_t{212}}) {
        auto bad = good; bad[offset] = 0xff; reject(bad);
    }
    auto bad = good; Put(bad, 4 + 0x48, 0xffffffff); reject(bad);
    bad = good; Put(bad, 4 + 0x10 + 4, 0x3fffff); reject(bad);
    bad = good; Put(bad, 4 + 0x2c + 4, 0); reject(bad);
    for (unsigned format = 0; format < 64; ++format) {
        if (format == 0 || format == 0x13) continue;
        bad = good; bad[13] = uint8_t(format); reject(bad);
    }
    bad = good; bad[14] = 0x14; reject(bad); // BUG-064: never pretend a T4 upload succeeded
    bad = good; bad.pop_back(); reject(bad);
    bad = good; bad.push_back(0); reject(bad);
    bad = Record(false); bad[12] = 2; reject(bad);
    // Tool pointers/unused fields are not identity or bounds; several retail
    // font assets carry CD fill in unused mip metadata.
    bad = good;
    std::fill(bad.begin() + 4 + 0x54, bad.begin() + 132, 0xcd);
    Codec::Image other;
    CHECK(Codec::Decode(bad.data(), bad.size(), other) == Codec::Result::Ok && Same(other, sentinel));
    SelfOracle(good);
    SelfOracle(Record(false));
    // BUG-021: changing only a mip's bytes must change that level. No base-only cache.
    bad = good; bad[132 + 96 + 16384] ^= 0xff;
    CHECK(Codec::Decode(bad.data(), bad.size(), other) == Codec::Result::Ok);
    CHECK(!Same(other, sentinel));
    SelfOracle(bad);
}
Queue::Hooks Hooks(Assets& assets) {
    Queue::Hooks hooks;
    hooks.upload = [&](const Queue::Upload& upload) { return assets.Upload(upload, true) == Assets::Result::Ok; };
    hooks.drain = [] { return true; };
    return hooks;
}
void OwnedLifetime() {
    TextureTestBackend backend;
    Resources resources(backend);
    Queue queue;
    Assets assets(queue, resources, backend);
    auto record = Record();
    Assets::View first;
    CHECK(assets.Load(0x100000, 19, record.data(), record.size(), first) == Assets::Result::Ok);
    CHECK(backend.creates == 0 && backend.uploads == 0);
    CHECK(queue.BeginFrame(41) == Queue::Result::Ok);
    CHECK(assets.Record(queue.End(0), first.token) == Assets::Result::Ok);
    record[132 + 96 + 16384] ^= 0xff;
    Assets::View second;
    CHECK(assets.Load(0x100000, 19, record.data(), record.size(), second) == Assets::Result::Ok);
    CHECK(second.token.generation != first.token.generation && second.lease != first.lease);
    CHECK(assets.Record(queue.End(1), second.token) == Assets::Result::Ok);
    CHECK(assets.Release(first.token) == Assets::Result::StaleGeneration);
    CHECK(assets.Release(second.token) == Assets::Result::Ok);
    Assets::View retained;
    CHECK(assets.Find(first.lease, retained) == Assets::Result::Ok && retained.image == first.image);
    CHECK(assets.Find(second.lease, retained) == Assets::Result::Ok && retained.image == second.image);
    CHECK(assets.Find(Queue::Lease{}, retained) == Assets::Result::NotOwned);
    Queue::Lease foreign(first.lease.get(), [](const Queue::Resource*) {});
    CHECK(assets.Find(foreign, retained) == Assets::Result::StaleGeneration);
    // Consumer lookup by texture item id + captured generation: each
    // retained generation stays addressable and immutable; a never-converted
    // id is MissingAsset; a generation of this id that was never allocated is
    // StaleGeneration; the id's current image is never substituted.
    Assets::View leased;
    CHECK(assets.Lease(19, first.token.generation, leased) == Assets::Result::Ok);
    CHECK(leased.image == first.image && leased.lease == first.lease && leased.asset_id == 19);
    CHECK(assets.Lease(19, second.token.generation, leased) == Assets::Result::Ok);
    CHECK(leased.image == second.image && leased.lease == second.lease);
    CHECK(assets.Lease(19, second.token.generation + 1, leased) == Assets::Result::StaleGeneration);
    CHECK(assets.Lease(20, first.token.generation, leased) == Assets::Result::MissingAsset);
    CHECK(leased.image == second.image); // failures leave the output unchanged
    retained = {}; foreign = {};
    std::fill(record.begin(), record.end(), 0xff); // no borrowed guest or archive bytes
    CHECK(assets.CollectAfterQuiescence() == Assets::Result::Ok && assets.Counts().versions == 2);
    CHECK(queue.Submit(Hooks(assets)) == Queue::Result::Ok);
    CHECK(backend.uploads == 2 && backend.level_uploads == 8 && backend.mip_finishes == 2 && backend.synthesized);
    // The chain beyond the base (the texsrc convention: test_texsrc pins last_mip_authored == 2 for
    // three levels): a 5-level image finishes at 4, never at the count including the base.
    CHECK(backend.last_mip_authored == int(second.image->levels.size()) - 1 && backend.last_mip_authored == 4);
    for (const auto* view : {&first, &second}) {
        ps2r::render::SurfaceId surface;
        CHECK(resources.Resolve(view->lease, &surface) == Resources::Result::Ok);
        CHECK(backend.At(surface).rgba == view->image->levels[0].rgba);
        for (size_t i = 1; i < view->image->levels.size(); ++i)
            CHECK(backend.mip_bytes[std::make_pair(surface.id, int(i))] == view->image->levels[i].rgba);
    }
    CHECK(queue.Counts().legacy_spans == 0 && assets.Counts().uploaded == 2);
    first = {}; second = {};
    // The consumer's lease alone keeps its generation alive through collection.
    CHECK(assets.CollectAfterQuiescence() == Assets::Result::Ok);
    CHECK(assets.Counts().versions == 1 && backend.destroys == 1);
    const auto held = leased.token.generation;
    CHECK(assets.Lease(19, held, leased) == Assets::Result::Ok && leased.token.generation == held);
    leased = {};
    CHECK(assets.CollectAfterQuiescence() == Assets::Result::Ok);
    CHECK(assets.Lease(19, held, leased) == Assets::Result::MissingAsset); // every version of 19 collected
    CHECK(assets.Counts().retained_bytes == 0 && backend.destroys == 2);
    CHECK(assets.Reset() == Assets::Result::Ok);
}
void RefusalAndFailure() {
    TextureTestBackend backend;
    Resources resources(backend);
    Queue queue;
    Assets assets(queue, resources, backend, {2, 1024 * 1024});
    const auto good = Record();
    Assets::View original, out;
    CHECK(assets.Load(0x100000, 1, good.data(), good.size(), original) == Assets::Result::Ok);
    out = original;
    auto bad = good; bad.resize(5);
    CHECK(assets.Load(0x100000, 2, bad.data(), bad.size(), out) == Assets::Result::InvalidAsset);
    CHECK(out.token.generation == original.token.generation && assets.Counts().loaded == 1);
    CHECK(assets.Find({0x100000, original.token.generation + 1}, out) == Assets::Result::StaleGeneration);
    CHECK(assets.Load(3, 1, good.data(), good.size(), out) == Assets::Result::InvalidObject);
    CHECK(queue.BeginFrame(0) == Queue::Result::Ok);
    CHECK(assets.Record(queue.End(0), original.token) == Assets::Result::Ok);
    backend.fail_mip = true;
    auto hooks = Hooks(assets);
    CHECK(queue.Submit(hooks) == Queue::Result::DispatchFailed);
    CHECK(assets.Counts().uploaded == 0 && assets.Counts().failed == 1);
    CHECK(assets.Find(original.token, out) == Assets::Result::BackendFailed);
    CHECK(assets.Reset() == Assets::Result::QueueRejected);
    CHECK(queue.Recover(hooks) == Queue::Result::Ok);
    CHECK(assets.Reset() == Assets::Result::Ok);
    original = {}; out = {};
    CHECK(assets.CollectAfterQuiescence() == Assets::Result::Ok && backend.destroys == 1);
}
void OriginalAssets(const std::filesystem::path& manifest) {
    std::ifstream list(manifest);
    CHECK(list.good());
    std::string name;
    while (std::getline(list, name)) {
        std::ifstream file(manifest.parent_path() / name, std::ios::binary);
        CHECK(file.good());
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        SelfOracle(bytes);
        if (oracle_assets % 500 == 0) {
            std::printf("SELF oracle progress: %llu assets\n", static_cast<unsigned long long>(oracle_assets));
            std::fflush(stdout);
        }
        if (failures) { std::fprintf(stderr, "asset: %s\n", name.c_str()); break; }
    }
}
}
int main(int argc, char** argv) {
    if (argc == 2) OriginalAssets(argv[1]);
    else { CodecValidation(); OwnedLifetime(); RefusalAndFailure(); }
    std::printf("TextureAssets: %d checks, %d failures; SELF %llu assets / %llu levels / %llu pixels\n",
        checks, failures, static_cast<unsigned long long>(oracle_assets),
        static_cast<unsigned long long>(oracle_levels), static_cast<unsigned long long>(oracle_pixels));
    return failures ? 1 : 0;
}
