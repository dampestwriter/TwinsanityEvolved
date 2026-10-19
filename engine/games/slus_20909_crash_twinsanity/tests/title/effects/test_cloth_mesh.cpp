#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_mesh.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_pass.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_fixture.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using Pass = title::MeshPass;
using Intent = title::MeshIntent;
using Queue = title::Graphics;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("cloth mesh line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
class TestClothMesh {
public:
    static constexpr uint32_t Object = 0x10000, View = 0x11000, List = 0x12000;
    static constexpr uint32_t Material = 0x13000, Shader = 0x14000, Image = 0x15000;
    static constexpr uint32_t Target = 0x16000, Model = 0x17000, Waves = 0x18000;
    EffectsFixture world;
    Queue queue;
    title::GraphicsTargets targets{queue};
    title::GraphicsScene::Binding binding;
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x400000);
    title::MeshAssets::ModelLease model;
    title::MeshMaterial::Texture texture;
    Queue::Handle image;
    std::map<uint32_t, Pass::Body> calls;
    std::unique_ptr<Pass> pass;
    unsigned original_geometry = 0, helpers = 0, reservations = 0, captures = 0, recorded = 0;
    bool reserve_ok = true, empty_geometry = false;
    std::vector<ps2r::gs::DrawIr> output;
    template<class T> void Put(uint32_t at, T value) { std::memcpy(memory.data() + at, &value, sizeof(T)); }
    auto Reader() {
        return [this](uint32_t at, void* out, size_t size) {
            if (at >= memory.size() || size > memory.size() - at) return false;
            std::memcpy(out, memory.data() + at, size);
            return true;
        };
    }
    auto Lookup() {
        return [this](uint32_t at, title::MeshMaterial::Texture& out) {
            if (at != Image) return false;
            out = texture;
            return true;
        };
    }
    explicit TestClothMesh(bool three = false, bool enabled = true) {
        Put(Object + 8, List); Put(Object + 12, Model); Put<uint32_t>(Object + 24, 1);
        Put(List, List + 16); Put(List + 24, Material); Put<uint32_t>(Model + 8, 1);
        Put(Material, Shader); Put<uint32_t>(Material + 16, 1);
        Put<uint64_t>(Material + 24, three ? 0x80000 : 0x10000);
        Put<uint32_t>(Material + 32, 2); Put<uint32_t>(Material + 0x60, 1);
        Put<uint64_t>(Shader, (uint64_t(7) << 26) | (uint64_t(2) << 21) | (uint64_t(1) << 55));
        Put(Shader + 20, Image); Put<uint32_t>(Shader + 0x64, three ? 0x1a : 0x17);
        const uint32_t table = three ? title::ClothMaterial::kThreeVtable : title::ClothMaterial::kSingleVtable;
        Put(Shader + 0x6c, table); Put<uint32_t>(table + 0x2c, three ? 0x1d0d20 : 0x1d18c8);
        Put(Shader + 0x270, Waves); Put(Shader + 0x27c, std::array<float, 3>{.5f, .5f, .5f});
        Wave(.25f);
        Put<uint32_t>(title::MaterialState::kDisplayWidth, 512);
        Put<uint32_t>(title::MaterialState::kDisplayHeight, 448);
        Put<uint32_t>(title::MaterialState::kDisplayPixelBytes, 2);
        Put<uint32_t>(title::MaterialState::kDepthBufferPage, 168);
        Put(title::MaterialState::kRenderTarget, Target);
        Put(Target, std::array<uint32_t, 8>{0, 0, 0, 512, 448, 0, 512, 448});
        title::MeshGeometry::Matrix screen{{{16,0,0,0}, {0,16,0,0}, {0,0,1,0}, {32768,32768,8388607,1}}};
        Put(View + 0x80, screen); Put<uint32_t>(View + 0xf8, 1);
        auto allocation = std::make_shared<title::MeshAssets::Model>();
        allocation->parts.resize(1);
        allocation->parts[0].strips.resize(1);
        auto& vertices = allocation->parts[0].strips[0].vertices;
        vertices.resize(3); vertices[1].position[0] = 1; vertices[2].position[1] = 1;
        for (auto& v : vertices) { v.stq = {0,0,1}; v.colour = {128,64,32,127}; }
        model = std::move(allocation);
        CHECK(queue.BeginFrame(world.frame.source_frame) == Queue::Result::Ok);
        CHECK(world.scene.Bind(queue, targets, world.frame, binding) == title::GraphicsScene::Result::Ok);
        CHECK(world.resources.Register(queue, {32,32,ps2r::render::SurfaceFormat::Rgba8,false,1}, &image) ==
              title::GraphicsResources::Result::Ok);
        CHECK(queue.RetainResource(image, &texture.lease) == Queue::Result::Ok);
        texture.handle = image; texture.width = texture.height = 32;
        texture.buffer_width = 64; texture.levels = 1; texture.source_format = 19; texture.alpha = true;
        CHECK(queue.RecordUpload(queue.End(1), image, {0,0,32,32}, 128,
              std::vector<uint8_t>(32 * 32 * 4, 127)) == Queue::Result::Ok);
        Pass::Hooks hooks;
        hooks.enabled = enabled;
        hooks.read = Reader();
        hooks.write = [this](const std::vector<Pass::Write>& writes) {
            for (const auto& write : writes)
                if (write.address >= memory.size() || write.bytes.size() > memory.size() - write.address) return false;
            for (const auto& write : writes)
                std::memcpy(memory.data() + write.address, write.bytes.data(), write.bytes.size());
            return true;
        };
        hooks.model = [this](uint32_t address) { return address == Model ? model : title::MeshAssets::ModelLease{}; };
        hooks.texture = Lookup();
        hooks.environment = [](title::MaterialState::Environment& out) { out.colour_clamp = true; return true; };
        hooks.owns_material = [](uint32_t address) { return address == Material; };
        hooks.capture_material = [this](uint32_t address, const title::MaterialState::Environment& environment,
                                       title::MeshMaterial::Output& out, Pass::Geometry& geometry) {
            ++captures;
            if (empty_geometry) return true; // Must be refused by the actual mesh owner.
            return title::ClothMesh::Capture(Reader(), address, environment, Lookup(), out, geometry) ==
                title::MeshMaterial::Result::Ok;
        };
        hooks.reserve = [this](const std::vector<Pass::Request>& requests, std::vector<Pass::Record>& records) {
            if (!reserve_ok) return false;
            for (const auto& request : requests) {
                CHECK(request.material == Material && request.shader.bucket == 2 && request.geometry);
                const auto resource = request.shader.texture.handle;
                const auto position = queue.End(request.shader.bucket);
                records.push_back([this, resource, position](const std::vector<ps2r::gs::DrawIr>& draws) {
                    ++recorded;
                    for (auto draw : draws) {
                        draw.src_frame = world.frame.source_frame;
                        CHECK(targets.RecordMesh(position, draw, resource) == title::GraphicsTargets::Result::Ok);
                        output.push_back(std::move(draw));
                    }
                    return true;
                });
                ++reservations;
            }
            // Source owners can recycle these immediately after reservation.
            // The captured geometry function and model block must own values.
            Wave(std::numeric_limits<float>::quiet_NaN());
            model.reset();
            return true;
        };
        pass = std::make_unique<Pass>(std::move(hooks));
        CHECK(pass->Install([this](uint32_t at) -> Pass::Body {
            return [this, at](CpuContext&, uint32_t) {
                if (at == Intent::kPlaced || at == Intent::kRigid || at == Intent::kFlushMaterial) ++original_geometry;
                else ++helpers;
            };
        }, [this](uint32_t at, Pass::Body body) { calls[at] = std::move(body); return true; }));
    }
    ~TestClothMesh() {
        pass.reset();
        targets.Reset();
        title::GraphicsScene::Release(queue, binding);
        texture = {};
        if (queue.CurrentState() == Queue::State::Faulted) queue.Recover(OutputHooks());
        queue.Reset();
        world.scene.CollectAfterQuiescence();
    }
    void Wave(float value) {
        for (unsigned row = 0; row < 16; ++row)
            Put(Waves + row * 16, std::array<float, 4>{value, value, value, 12345});
    }
    void Call(uint32_t entry) {
        CpuContext context{};
        context.cpuRegs.GPR.r[4].UL[0] = entry == Intent::kFlushMaterial ? Material : Object;
        context.cpuRegs.GPR.r[5].UL[0] = View;
        calls.at(entry)(context, entry);
    }
    Queue::Hooks OutputHooks() {
        Queue::Hooks hooks;
        hooks.upload = [this](const Queue::Upload& upload) {
            return world.resources.Upload(upload) == title::GraphicsResources::Result::Ok;
        };
        hooks.draw = [this](const ps2r::gs::DrawIr& draw, const std::vector<Queue::Lease>& leases) {
            return world.scene.Draw(draw, leases) == title::GraphicsScene::Result::Ok;
        };
        hooks.drain = [] { return true; };
        return hooks;
    }
};
}
int main() {
    try {
        for (bool three : {false, true}) {
            TestClothMesh f(three);
            const std::vector<uint8_t> mirror(f.world.memory.Bytes(),
                f.world.memory.Bytes() + ps2r::gs::VramImage::kBytes);
            for (unsigned i = 0; i < 24; ++i) f.Call(Intent::kPlaced);
            CHECK(f.pass->Stats().placed.replaced == 24 && f.pass->Stats().blocks == 2);
            CHECK(!f.original_geometry && !f.recorded && !f.reservations);
            f.Wave(.75f); // Source frame update after collection, before flush.
            f.Call(Intent::kClose); f.Call(Intent::kFlushMaterial);
            CHECK(f.reservations == 1 && f.recorded == 1 && f.output.size() == 24 && f.captures == 25);
            CHECK(f.output.front().verts.front().x == (three ? 256.25f : 256.375f));
            size_t vertices = 0;
            for (const auto& draw : f.output) vertices += draw.verts.size();
            CHECK(vertices == 72);
            f.Call(Pass::kForget);
            CHECK(f.helpers == 2 && !f.original_geometry && !f.pass->Stats().faults);
            CHECK(f.queue.ReleaseResource(f.image) == Queue::Result::Ok);
            f.texture = {};
            f.memory.assign(f.memory.size(), 0); // No source reads during delivery.
            f.world.pipeline.SetGfxEpoch(789);
            CHECK(f.queue.Submit(f.OutputHooks()) == Queue::Result::Ok);
            CHECK(f.queue.Counts().draws == 24 && f.queue.Counts().uploads == 1 &&
                  f.queue.Counts().legacy_spans == 0 && f.queue.Counts().replaced_bucket_jobs[2] == 24);
            CHECK(f.world.backend.draws == 24 && f.world.backend.uploads == 1 && f.world.backend.readbacks == 0);
            CHECK(f.world.pipeline.DrawSeq() == 24 && f.world.pipeline.GfxEpoch() == 789);
            CHECK(std::memcmp(mirror.data(), f.world.memory.Bytes(), mirror.size()) == 0);
        }
        { TestClothMesh f(false, false); f.Call(Intent::kPlaced);
          CHECK(f.original_geometry == 1 && !f.captures && !f.reservations); }
        { TestClothMesh f; f.empty_geometry = true; f.Call(Intent::kPlaced);
          CHECK(f.original_geometry == 1 && !f.pass->Stats().placed.replaced && !f.reservations); }
        { TestClothMesh f; f.Call(Intent::kPlaced); f.Call(Intent::kClose); f.reserve_ok = false;
          bool threw = false; try { f.Call(Intent::kFlushMaterial); } catch (const std::runtime_error&) { threw = true; }
          CHECK(threw && !f.original_geometry && !f.recorded && f.pass->Stats().faults == 1); }
        std::printf("ClothMesh: %u checks; source collector -> ordered Graphics -> shared scene, both cloth families\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
