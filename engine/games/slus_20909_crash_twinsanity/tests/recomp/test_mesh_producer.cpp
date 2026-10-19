// MeshProducer -- the root composition of the owned mesh bucket
// over fake services: the MaterialResourceRead receipts and the certificate's
// verdicts, the texture header/asset agreement, the ModelRead/ModelDestroy
// observers, the FlushMaterial reservation and its slot delivery. The pass's
// own arithmetic/order is MeshPassSelfOracle's; the certified path needs the
// retail record bytes and is the Root pin's (RootMeshLevelEntry).
#include "games/slus_20909_crash_twinsanity/composition/mesh_producer.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "engine/core/hash/sha256.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/material_certificate.h"

namespace {
using namespace ps2r;
using Producer = app::MeshProducer;
using Graphics = title::Graphics;
using View = title::TextureAssets::View;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (false)

struct Fixture {
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x400000);
    std::map<uint32_t, Producer::Body> bodies;
    std::map<uint32_t, unsigned> originals;
    std::map<uint32_t, View> textures;          // object -> the owner's published View
    std::map<const Graphics::Resource*, View> leases;
    std::vector<std::pair<uint32_t, Producer::Work>> queued;
    std::vector<std::vector<app::GraphicsProducerDraws::AssetDraw>> drawn;
    bool environment_ok = true, queue_ok = true, draw_ok = true;
    std::vector<uint32_t> refusals;
    std::unique_ptr<Producer> producer;
    uint32_t stream_advance = 0;   // bytes the fake MaterialResourceRead consumes
    uint32_t publish = 0x4000;     // the Material* the fake MaterialResourceRead publishes at resource+8

    static constexpr uint32_t kStream = 0x1000, kRecord = 0x2000, kResource = 0x3000, kMaterial = 0x4000;
    static constexpr uint32_t kShader = 0x5000, kTexture = 0x6000, kMemoryStream = 0x306b50;

    template<class T> void Put(uint32_t at, const T& v) { std::memcpy(ram.data() + at, &v, sizeof v); }
    template<class T> T Get(uint32_t at) const { T v; std::memcpy(&v, ram.data() + at, sizeof v); return v; }

    explicit Fixture(bool enabled = true) {
        Producer::Services services;
        services.read = [this](uint32_t at, void* out, size_t size) {
            if (at >= ram.size() || size > ram.size() - at) return false;
            std::memcpy(out, ram.data() + at, size);
            return true;
        };
        services.write = [this](const std::vector<title::MeshPass::Write>& writes) {
            for (const auto& w : writes) if (w.address + w.bytes.size() > ram.size()) return false;
            for (const auto& w : writes) std::memcpy(ram.data() + w.address, w.bytes.data(), w.bytes.size());
            return true;
        };
        services.texture = [this](uint32_t object, View& out) {
            const auto found = textures.find(object);
            if (found == textures.end()) return false;
            out = found->second;
            return true;
        };
        services.view = [this](const Graphics::Lease& lease, View& out) {
            const auto found = leases.find(lease.get());
            if (found == leases.end()) return false;
            out = found->second;
            return true;
        };
        services.environment = [this](uint32_t bucket, Producer::Frame& frame, Producer::Environment& env) {
            if (!environment_ok || !title::FrameEnvironment::MeshBucket(bucket)) return false;
            frame = {0x70, 0xa0, 512, 448, 7, 3};
            env.colour_clamp = true;
            return true;
        };
        services.queue = [this](uint32_t bucket, Producer::Work work) {
            if (!queue_ok) return false;
            queued.emplace_back(bucket, std::move(work));
            return true;
        };
        services.draw = [this](const Producer::Frame& frame, const auto& batch, uint32_t* why) {
            CHECK(frame.source_frame == 7 && frame.width == 512);
            if (!draw_ok) { if (why) *why = 0x40; return false; }
            drawn.push_back(batch);
            return true;
        };
        services.refuse = [this](auto, uint32_t, uint32_t detail) { refusals.push_back(detail); };
        producer = std::make_unique<Producer>(enabled, std::move(services));
        // The title's memory Stream over a record at kRecord.
        Put<uint32_t>(kStream, kMemoryStream);
        Put<uint32_t>(kStream + 8, kRecord);
        Put<uint32_t>(kStream + 12, kRecord);
        Put<uint32_t>(kStream + 16, 0x1000);
        for (uint32_t i = 0; i < 0x200; ++i) ram[kRecord + i] = uint8_t(i * 7 + 3);
        Put<uint32_t>(kResource + 4, 0x0003e763);   // the catalogue's first material id
        // The material object: shader pointer at +0, bucket at +0x20; the shader's texture object at +0x14.
        Put<uint32_t>(kMaterial, kShader);
        Put<uint32_t>(kMaterial + 0x20, 2);
        Put<uint32_t>(kShader + 0x14, kTexture);
        Put<uint32_t>(kTexture + 4, 0x4308e70f);     // the entry's texture id
    }
    bool Install() {
        const auto capture = [this](uint32_t address) -> Producer::Body {
            if (!bodies.count(address)) bodies[address] = [this, address](CpuContext& context, uint32_t) {
                ++originals[address];
                if (address == Producer::kMaterialResourceRead) {
                    // MaterialResourceRead: consume the record, publish Material* at resource+8.
                    Put<uint32_t>(kStream + 12, Get<uint32_t>(kStream + 12) + stream_advance);
                    Put<uint32_t>(context.cpuRegs.GPR.r[4].UL[0] + 8, publish);
                }
            };
            return bodies.at(address);
        };
        const auto install = [this](uint32_t address, Producer::Body body) { bodies[address] = std::move(body); return true; };
        return producer->Install(capture, install);
    }
    void Call(uint32_t address, uint32_t a0, uint32_t a1 = 0, uint32_t entry = 0) {
        CpuContext context{};
        context.cpuRegs.GPR.r[4].UL[0] = a0;
        context.cpuRegs.GPR.r[5].UL[0] = a1;
        bodies.at(address)(context, entry);
    }
    View MakeView(uint32_t width, uint32_t height, size_t levels, uint32_t object) {
        View view;
        auto resource = std::make_shared<Graphics::Resource>();
        resource->description = {width, height, render::SurfaceFormat::Rgba8, false, 1};
        resource->lifetime = std::make_shared<int>(1);
        view.lease = resource;
        view.token = {object, 1};
        view.asset_id = Get<uint32_t>(object + 4);
        auto image = std::make_shared<title::TextureCodec::Image>();
        for (size_t i = 0; i < levels; ++i)
            image->levels.push_back({width >> i, height >> i, std::vector<uint8_t>(size_t(width >> i) * (height >> i) * 4)});
        view.image = image;
        leases[resource.get()] = view;
        return view;
    }
};
}  // namespace

int main() {
    // Receipts: the bytes the original consumes are hashed for the Material* it publishes.
    {
        Fixture f;
        CHECK(f.Install());
        CHECK(f.producer->Enabled());
        f.stream_advance = 0x90;
        f.Call(Producer::kMaterialResourceRead, Fixture::kResource, Fixture::kStream);
        auto counts = f.producer->Counts();
        CHECK(f.originals[Producer::kMaterialResourceRead] == 1);
        CHECK(counts.material_reads == 1 && counts.receipts == 1 && counts.unhashed == 0 && counts.receipts_live == 1);
        // The record digest is the live bytes' (the catalogue holds a different record: UnknownRecord).
        CHECK(hash::Sha256::HexOf(f.ram.data() + Fixture::kRecord, 0x90) != title::MaterialCertificate::At(0)->record_sha256);
        CHECK(!f.producer->OwnsMaterial(Fixture::kMaterial));
        counts = f.producer->Counts();
        CHECK(counts.owns_calls == 1 && counts.unknown_record == 1 && counts.certified == 0);
        CHECK(f.refusals.size() == 1 && f.refusals[0] == 3);
        // A material without a receipt declines before the certificate.
        CHECK(!f.producer->OwnsMaterial(0x7000));
        CHECK(f.producer->Counts().no_receipt == 1);
        // The shader's identity bytes changed under the receipt (a reused address): stale, declined;
        // the colour/scroll block animating (AnimateMaterial) is not identity.
        f.Put<uint32_t>(Fixture::kShader + 0x10, 0x55);
        CHECK(!f.producer->OwnsMaterial(Fixture::kMaterial) && f.producer->Counts().receipt_stale == 1);
        CHECK(f.refusals.back() == 13);
        f.Put<uint32_t>(Fixture::kShader + 0x10, 0);
        f.Put<float>(Fixture::kShader + 0x50, 0.25f);
        CHECK(!f.producer->OwnsMaterial(Fixture::kMaterial) && f.producer->Counts().receipt_stale == 1 &&
              f.producer->Counts().unknown_record == 2);
        // An unknown id: the certificate has never heard of it.
        f.Put<uint32_t>(Fixture::kResource + 4, 0x12345678);
        f.Put<uint32_t>(Fixture::kStream + 12, Fixture::kRecord);
        f.Call(Producer::kMaterialResourceRead, Fixture::kResource, Fixture::kStream);
        CHECK(!f.producer->OwnsMaterial(Fixture::kMaterial) && f.producer->Counts().unknown_material == 1);
        // A read whose stream is not the title's memory Stream, or that consumed nothing, leaves no receipt.
        f.Put<uint32_t>(Fixture::kStream, 0xdeadbeef);
        f.Call(Producer::kMaterialResourceRead, Fixture::kResource, Fixture::kStream);
        counts = f.producer->Counts();
        CHECK(counts.unhashed == 1 && counts.receipts_live == 0);
        CHECK(!f.producer->OwnsMaterial(Fixture::kMaterial) && f.producer->Counts().no_receipt == 2);
        f.Put<uint32_t>(Fixture::kStream, Fixture::kMemoryStream);
        f.stream_advance = 0;
        f.Call(Producer::kMaterialResourceRead, Fixture::kResource, Fixture::kStream);
        CHECK(f.producer->Counts().unhashed == 2);
        // An interior entry observes nothing.
        f.stream_advance = 0x90;
        f.Call(Producer::kMaterialResourceRead, Fixture::kResource, Fixture::kStream, Producer::kMaterialResourceRead + 8);
        CHECK(f.producer->Counts().material_reads == 4 && f.originals[Producer::kMaterialResourceRead] == 5);
    }
    // Disabled: the observers run the originals and record nothing.
    {
        Fixture f(false);
        CHECK(f.Install());
        CHECK(!f.producer->Enabled());
        f.stream_advance = 0x90;
        f.Call(Producer::kMaterialResourceRead, Fixture::kResource, Fixture::kStream);
        f.Call(Producer::kModelRead, 0x8000);
        f.Call(Producer::kModelDestroy, 0x8000);
        const auto counts = f.producer->Counts();
        CHECK(counts.material_reads == 0 && counts.receipts == 0 && counts.model_reads == 0 && counts.model_destroys == 0);
        CHECK(f.originals[Producer::kMaterialResourceRead] == 1 && f.originals[Producer::kModelRead] == 1 &&
              f.originals[Producer::kModelDestroy] == 1);
        CHECK(!f.producer->Model(0x8000) && f.producer->Counts().lazy_loads == 0);
    }
    // ModelRead: an object the converter cannot take is declined (counted by result); Destroy forgets.
    {
        Fixture f;
        CHECK(f.Install());
        f.Call(Producer::kModelRead, 0x8000);   // zero part count: UnsupportedAsset
        auto counts = f.producer->Counts();
        CHECK(counts.model_reads == 1 && counts.models_unsupported == 1 && counts.models_loaded == 0);
        f.Call(Producer::kModelRead, 0x3ffff0);   // past the readable window: InvalidMemory
        CHECK(f.producer->Counts().models_invalid == 1);
        CHECK(!f.producer->Model(0x8000));   // lazily retried, still declined
        counts = f.producer->Counts();
        CHECK(counts.lazy_loads == 1 && counts.lazy_misses == 1);
        f.Call(Producer::kModelDestroy, 0x8000);
        CHECK(f.producer->Counts().model_destroys == 1 && f.originals[Producer::kModelDestroy] == 1);
    }
    // Texture: the owner's View and the GameTexture header must agree.
    {
        Fixture f;
        CHECK(f.Install());
        f.Put<uint16_t>(Fixture::kTexture + 16, 6);   // 64
        f.Put<uint16_t>(Fixture::kTexture + 18, 5);   // 32
        f.Put<uint8_t>(Fixture::kTexture + 20, 3);    // levels
        f.Put<uint8_t>(Fixture::kTexture + 21, 19);   // PSMT8
        f.Put<uint8_t>(Fixture::kTexture + 23, 1);    // alpha
        f.Put<uint8_t>(Fixture::kTexture + 25, 1);    // function
        f.Put<uint32_t>(Fixture::kTexture + 0x38, 1); // buffer width 64
        title::MeshMaterial::Texture texture;
        CHECK(!f.producer->Texture(Fixture::kTexture, texture) && f.producer->Counts().texture_missing == 1);
        f.textures[Fixture::kTexture] = f.MakeView(64, 32, 3, Fixture::kTexture);
        CHECK(f.producer->Texture(Fixture::kTexture, texture));
        CHECK(texture.width == 64 && texture.height == 32 && texture.levels == 3 && texture.source_format == 19 &&
              texture.alpha && texture.function == 1 && texture.buffer_width == 64 &&
              texture.lease == f.textures[Fixture::kTexture].lease);
        f.Put<uint8_t>(Fixture::kTexture + 20, 2);    // the header disagrees with the asset's level count
        CHECK(!f.producer->Texture(Fixture::kTexture, texture) && f.producer->Counts().texture_mismatch == 1);
        f.Put<uint8_t>(Fixture::kTexture + 20, 3);
        f.Put<uint16_t>(Fixture::kTexture + 16, 7);   // ... or its size
        CHECK(!f.producer->Texture(Fixture::kTexture, texture) && f.producer->Counts().texture_mismatch == 2);
        CHECK(f.producer->Counts().texture_lookups == 4);
    }
    // Reserve: the bucket's frame + the pinned View, the slot taken, the record filling it, the delivery.
    {
        Fixture f;
        CHECK(f.Install());
        f.Put<uint16_t>(Fixture::kTexture + 16, 5);
        f.Put<uint16_t>(Fixture::kTexture + 18, 5);
        f.Put<uint8_t>(Fixture::kTexture + 20, 1);
        f.Put<uint32_t>(Fixture::kTexture + 0x38, 1);
        f.textures[Fixture::kTexture] = f.MakeView(32, 32, 1, Fixture::kTexture);
        title::MeshPass::Request request{Fixture::kMaterial, {}};
        CHECK(f.producer->Texture(Fixture::kTexture, request.shader.texture));
        request.shader.bucket = 2;
        std::vector<title::MeshPass::Record> records;
        // Refusals before commit: the environment, the view, the slot.
        f.environment_ok = false;
        CHECK(!f.producer->Reserve({request}, records) && records.empty() && f.queued.empty());
        f.environment_ok = true;
        auto unknown = request;
        unknown.shader.texture.lease = std::make_shared<Graphics::Resource>();
        CHECK(!f.producer->Reserve({unknown}, records) && records.empty() && f.queued.empty());
        f.queue_ok = false;
        CHECK(!f.producer->Reserve({request}, records) && records.empty());
        f.queue_ok = true;
        auto counts = f.producer->Counts();
        CHECK(counts.requests == 3 && counts.refused_environment == 1 && counts.refused_view == 1 &&
              counts.refused_slot == 1 && counts.reserved == 0);
        CHECK(f.refusals.size() == 3 && f.refusals[0] == 10 && f.refusals[1] == 11 && f.refusals[2] == 12);
        // A bucket the environment hook does not certify refuses the same way.
        auto ui = request;
        ui.shader.bucket = 24;
        CHECK(!f.producer->Reserve({ui}, records) && f.producer->Counts().refused_environment == 2);
        // The reservation: one slot at bucket 2; delivered before its record -> refused kUnfilled.
        CHECK(f.producer->Reserve({request}, records));
        CHECK(records.size() == 1 && f.queued.size() == 1 && f.queued[0].first == 2);
        CHECK(f.producer->Counts().reserved == 1);
        uint32_t why = 0;
        CHECK(!f.queued[0].second(why) && why == Producer::kUnfilled && f.drawn.empty());
        // The record fills the slot once; the delivery draws exactly those draws with the pinned View.
        std::vector<gs::DrawIr> draws(3);
        draws[0].prim_type = 4; draws[1].prim_type = 4; draws[2].prim_type = 4;
        draws[2].vertex_count = 9;
        CHECK(records[0](draws));
        CHECK(!records[0](draws));   // a second record of the same reservation is refused
        CHECK(f.producer->Counts().delivered_draws == 3);
        why = 0;
        CHECK(f.queued[0].second(why) && why == 0 && f.drawn.size() == 1 && f.drawn[0].size() == 3);
        CHECK(f.drawn[0][2].draw.vertex_count == 9 && f.drawn[0][0].texture.lease == request.shader.texture.lease);
        // A refusing draw at the slot reports its detail.
        f.draw_ok = false;
        why = 0;
        CHECK(!f.queued[0].second(why) && why == 0x40);
        std::string reason;
        CHECK(f.producer->CanCheckpoint(&reason));
        f.producer->ResetAfterQuiescence();
        CHECK(f.producer->CanCheckpoint(&reason));
    }
    // Receipts carried across a restore (MSHR): the capture round-trips into a fresh owner over the
    // same image; a receipt whose Material now reads other identity bytes is dropped; the frame
    // admission walks the title's material table and refuses a frame whole while a listed material
    // has no receipt (the section-less image), admitting it once every listed material has one.
    {
        Fixture f;
        CHECK(f.Install());
        f.stream_advance = 0x90;
        f.Call(Producer::kMaterialResourceRead, Fixture::kResource, Fixture::kStream);
        // A second material at another address with its own record.
        enum : uint32_t { kMaterial2 = 0x4100, kResource2 = 0x3100, kShader2 = 0x5100 };
        f.Put<uint32_t>(kMaterial2, kShader2);
        f.Put<uint32_t>(kMaterial2 + 0x20, 3);
        f.Put<uint32_t>(kShader2 + 0x14, Fixture::kTexture);
        f.Put<uint32_t>(kResource2 + 4, 0x00081eab);
        f.publish = kMaterial2;
        f.Call(Producer::kMaterialResourceRead, kResource2, Fixture::kStream);
        CHECK(f.producer->Counts().receipts_live == 2);
        std::vector<uint8_t> payload;
        CHECK(f.producer->CaptureReceipts(payload));
        std::string error;
        CHECK(Producer::ValidateReceipts(payload, &error));
        CHECK(payload.size() == 4 + f.producer->Counts().receipts_live * 48);
        // Malformed payloads are refused by shape.
        auto broken = payload;
        broken.pop_back();
        CHECK(!Producer::ValidateReceipts(broken, &error) && !error.empty());
        CHECK(!Producer::ValidateReceipts({}, &error));
        // The frame admission / restore read the title's material table.
        enum : uint32_t { kEntries = 0x9000, kItem = 0x9100, kItem2 = 0x9120 };
        const auto table = [](Fixture& x, uint16_t count, uint32_t vtable) {
            x.Put<uint32_t>(Producer::kMaterialTable, kEntries);
            x.Put<uint16_t>(Producer::kMaterialTable + 4, count);
            x.Put<uint16_t>(Producer::kMaterialTable + 6, 640);
            x.Put<uint32_t>(Producer::kMaterialTable + 0x14, vtable);
            x.Put<uint32_t>(kEntries, kItem); x.Put<uint32_t>(kEntries + 4, 0x0003e763);
            x.Put<uint32_t>(kEntries + 8, kItem2); x.Put<uint32_t>(kEntries + 12, 0x00081eab);
            x.Put<uint32_t>(kItem + 4, 0x0003e763); x.Put<uint32_t>(kItem + 8, Fixture::kMaterial);
            x.Put<uint32_t>(kItem2 + 4, 0x00081eab); x.Put<uint32_t>(kItem2 + 8, kMaterial2);
        };
        // An image without the table restores nothing (every receipt dropped), admits no frame.
        Fixture e;
        CHECK(e.Install());
        e.ram = f.ram;
        CHECK(e.producer->RestoreReceipts(payload, [&e](uint32_t at, void* out, size_t size) {
            if (at >= e.ram.size() || size > e.ram.size() - at) return false;
            std::memcpy(out, e.ram.data() + at, size);
            return true;
        }, &error));
        CHECK(e.producer->Counts().receipts_dropped == 2 && e.producer->Counts().receipts_live == 0);
        CHECK(!e.producer->FrameAdmits() && e.producer->Counts().table_unreadable == 1 &&
              e.producer->Counts().frames_whole_model == 1);
        // The fresh owner over the same image (table bound) holds the same receipts and certifies/declines identically.
        Fixture g;
        CHECK(g.Install());
        g.ram = f.ram;
        table(g, 2, Producer::kMaterialTableVtable);
        CHECK(g.producer->Counts().receipts_live == 0);
        CHECK(g.producer->RestoreReceipts(payload, [&g](uint32_t at, void* out, size_t size) {
            if (at >= g.ram.size() || size > g.ram.size() - at) return false;
            std::memcpy(out, g.ram.data() + at, size);
            return true;
        }, &error));
        auto counts = g.producer->Counts();
        CHECK(counts.receipts_live == f.producer->Counts().receipts_live && counts.receipts_dropped == 0 &&
              counts.receipts_restored == counts.receipts_live);
        CHECK(!g.producer->OwnsMaterial(Fixture::kMaterial) && g.producer->Counts().unknown_record == 1 &&
              g.producer->Counts().no_receipt == 0);
        // The image's table binds a receipt's address to another resource id (the address reused): dropped.
        // Shader identity words that moved since the read are NOT a drop: the hash is re-taken from the image.
        Fixture h;
        CHECK(h.Install());
        h.ram = f.ram;
        table(h, 2, Producer::kMaterialTableVtable);
        h.Put<uint32_t>(kEntries + 4, 0x00c1f063);
        h.Put<uint32_t>(kItem + 4, 0x00c1f063);
        h.Put<uint32_t>(kShader2 + 0x10, 0x77);
        CHECK(h.producer->RestoreReceipts(payload, [&h](uint32_t at, void* out, size_t size) {
            if (at >= h.ram.size() || size > h.ram.size() - at) return false;
            std::memcpy(out, h.ram.data() + at, size);
            return true;
        }, &error));
        counts = h.producer->Counts();
        CHECK(counts.receipts_dropped == 1 && counts.receipts_restored + 1 == f.producer->Counts().receipts_live);
        CHECK(!h.producer->OwnsMaterial(Fixture::kMaterial) && h.producer->Counts().no_receipt == 1);
        CHECK(!h.producer->OwnsMaterial(kMaterial2) && h.producer->Counts().receipt_stale == 0 &&
              h.producer->Counts().unknown_record == 1);   // the re-taken identity holds
        // The frame admission over the title's material table: every listed material has a receipt -> admitted.
        CHECK(g.producer->FrameAdmits() && g.producer->Counts().frames_admitted == 1 &&
              g.producer->Counts().unreceipted_live == 0);
        // The owner whose receipt was dropped: one listed material lacks one -> whole-model frame.
        CHECK(!h.producer->FrameAdmits() && h.producer->Counts().frames_whole_model == 1 &&
              h.producer->Counts().unreceipted_live == 1 && h.producer->Counts().frames_admitted == 0);
        // A table with a foreign vtable is unreadable: refused whole.
        table(h, 2, 0x1234);
        CHECK(!h.producer->FrameAdmits() && h.producer->Counts().table_unreadable == 1);
        // An empty entry (destroyed material) is skipped; an empty table admits.
        table(h, 0, Producer::kMaterialTableVtable);
        CHECK(h.producer->FrameAdmits() && h.producer->Counts().frames_admitted == 1);
        // The mixed-frame judgement: an admitted frame with a receipt refusal and no replaced boundary is
        // not mixed; the gate never admits such a frame in play, this is the counter's own contract.
        CHECK(!h.producer->OwnsMaterial(Fixture::kMaterial));
        CHECK(h.producer->FrameAdmits() && h.producer->Counts().mixed_frames == 0);
        // The pass hook is wired: the pass's frame counters follow the admission on a model boundary.
        {
            Fixture k;
            CHECK(k.Install());
            table(k, 1, Producer::kMaterialTableVtable);   // kMaterial listed, no receipt in k
            k.Call(title::MeshIntent::kPlaced, 0x0);
            const auto stats = k.producer->Counts();
            CHECK(stats.pass.frames_whole_model == 1 && stats.pass.frames_admitted == 0 &&
                  stats.pass.placed.model_calls == 1 && stats.pass.placed.replaced == 0 && stats.no_receipt == 0);
            CHECK(k.originals[title::MeshIntent::kPlaced] == 1);
            k.Call(title::MeshIntent::kPlaced, 0x0);
            CHECK(k.producer->Counts().pass.frames_whole_model == 1);   // decided once per frame
            k.Call(title::MeshPass::kForget, 0x0);
            k.Call(title::MeshIntent::kPlaced, 0x0);
            CHECK(k.producer->Counts().pass.frames_whole_model == 2);   // the next frame decides again
        }
    }
    std::printf("MeshProducer: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
