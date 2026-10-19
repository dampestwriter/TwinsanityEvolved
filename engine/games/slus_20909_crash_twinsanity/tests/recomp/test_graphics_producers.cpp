#include "games/slus_20909_crash_twinsanity/composition/graphics_producers.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_census.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

#include "engine/ee/mem/main_memory.h"
#include "tests/lib/fake_backend.h"
#include "games/slus_20909_crash_twinsanity/tests/title/text/text_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_test_data.h"

namespace {
using namespace ps2r;
using Producers = app::GraphicsProducers;
using Draws = app::GraphicsProducerDraws;
using Graphics = title::Graphics;
using Frame = title::FrameEnvironment;
using Scene = title::GraphicsScene;
using Assets = title::TextureAssets;
using Movie = title::MovieGraphics;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (false)
void Throws(const std::function<void()>& call) {
    bool threw = false;
    try { call(); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
class TestGraphicsProducers {
public:
    struct Backend : test::FakeBackend {
        struct Call { gs::DrawIr draw; gs::TextureSource texture; render::DrawParams params; int uploads; };
        std::vector<Call> calls;
        bool fail_draw = false;
        render::DrawResult Draw(const gs::DrawIr& d, const gs::TextureSource& t,
                                const render::DrawParams& p) override {
            if (fail_draw) throw std::runtime_error("backend refused selected draw");
            calls.push_back({d,t,p,uploads});
            if (p.do_frame_clear)
                Clear(p.color,p.depth,0,0,render::kClearDepth | (p.clear_color ? uint32_t{render::kClearColor} : 0u));
            return FakeBackend::Draw(d,t,p);
        }
    };

    Backend backend;
    title::GraphicsSurfaces surfaces{backend};
    gs::VramImage vram;
    gs::VramLayout layout;
    gs::ScalePolicy scale{gs::ScaleSettings{2}, &layout};
    gs::RenderTargetCache cache{surfaces,vram,scale};
    gs::TargetRefresher refresher{surfaces,vram,cache};
    gs::TextureResolver resolver{surfaces,vram,cache,refresher,layout};
    gs::ScanoutComposer scanout{surfaces,cache};
    gs::Presenter presenter{scanout};
    gs::SequenceRecipe sequence{gs::SequenceRecipe::Constants{}};
    gs::DrawPipeline pipeline{gs::DrawPipeline::Deps{
        &surfaces,&vram,&cache,&refresher,&resolver,&scanout,&presenter,&layout,nullptr,&sequence}};
    title::GraphicsResources resources{surfaces};
    title::GraphicsRenderer renderer{surfaces,resources};
    Scene scene{surfaces,resources,renderer,pipeline};
    Graphics texture_queue;
    Assets assets{texture_queue,resources,surfaces};
    MainMemory ram;
    title::TextureSelection selection;
    title::TextureBindings bindings;
    Draws draws{resources,renderer,scene,assets,false};
    Frame frame;
    Scene::Frame captured;
    std::map<uint32_t, Producers::Body> bodies;
    std::map<uint32_t, unsigned> model;
    std::vector<std::pair<uint32_t,std::function<bool()>>> pending;
    std::unique_ptr<Producers> producers;
    title::GraphicsRefusals refusals;
    bool accept = true, source_ready = true, late_refusal = false;
    static constexpr uint32_t target = 0x160000, sprite = 0x170000;
    static constexpr uint32_t count = 0x30a810, list = 0x3d5cf0;
    explicit TestGraphicsProducers(Producers::Options options = {})
        : selection(assets,{ram.Data(),ram.Size()},{options.text,options.sprites,options.textures},&refusals),
          bindings(assets,{ram.Data(),ram.Size()},[this](auto object,auto id) {
              return selection.Texture(object,id);
          }), frame([this](auto address,void* out,auto size) { return Read(address,out,size); }) {
        TextureBindingFixture initial;
        std::memcpy(ram.Data(),initial.ram.data(),initial.ram.size());
        Display();
        pipeline.OnSetWindow(512,448,512,448);
        pipeline.OnSetFrontBuffer(0x70000,512,0);
        Producers::Output output;
        output.ui = [this](auto bucket,auto& out,auto& environment) {
            return source_ready && frame.CaptureScene(bucket,out) && frame.Read(environment);
        };
        output.sky = [this](auto& out,auto& environment) {
            return source_ready && frame.CaptureSky(out,environment);
        };
        output.movie = [this](auto& out) { return source_ready && frame.CaptureScene(27,out); };
        output.record = [this](auto bucket,auto work) {
            if (!accept) return false;
            pending.emplace_back(bucket,std::move(work)); return !late_refusal;
        };
        output.immediate = [this](auto work) {
            if (!accept) return false;
            pending.emplace_back(UINT32_MAX,std::move(work)); return !late_refusal;
        };
        output.refusals = &refusals;
        producers = std::make_unique<Producers>(ram,draws,bindings,selection,options,std::move(output));
        const auto capture = [this](uint32_t address) {
            if (!bodies.count(address)) bodies[address] = [this,address](CpuContext&,uint32_t) {
                ++model[address];
                if (address == 0x1c0410) {
                    for (uint32_t n = 0; n < Word(count); ++n) Put(Word(list+n*4)+0x64,0,1);
                    Put(count,0);
                }
            };
            return bodies.at(address);
        };
        const auto install = [this](auto address,auto body) { bodies[address]=std::move(body); return true; };
        CHECK(producers->Install(capture,install));
        CHECK(frame.Install(capture,install)); // outer source observer
        Call(Frame::kSetUpFrameAddress,target);
        CHECK(frame.CaptureScene(27,captured));
    }
    ~TestGraphicsProducers() {
        // Failed selected output is fatal, but every accepted transport item
        // must still run once to release its accounting/lifetime before teardown.
        Drain(false);
    }
    bool Read(uint32_t at,void* out,size_t size) {
        const auto* p = ram.Pointer(at & 0x1fffffff,size);
        if (!p) return false;
        std::memcpy(out,p,size); return true;
    }
    void Put(uint32_t at,uint64_t value,unsigned size=4) {
        auto* p = ram.Pointer(MainMemory::Phys(at),size);
        if (!p) throw std::runtime_error("fixture extent");
        for (unsigned i=0;i<size;++i) p[i]=uint8_t(value>>(i*8));
    }
    uint32_t Word(uint32_t at) const { return TextureTestData::Word(ram.Pointer(MainMemory::Phys(at),4)); }
    void Float(uint32_t at,float value) { uint32_t bits; std::memcpy(&bits,&value,4); Put(at,bits); }
    void Display() {
        Put(0x30a764,512); Put(0x30a768,448); Put(0x309830,2); Put(0x309834,4);
        Put(0x30a77c,56); Put(0x30a780,168); Put(0x309744,512); Put(0x309748,448);
        Put(0x30983c,target); Put(target+0xc,512); Put(target+0x10,448);
        Put(target+0x18,512); Put(target+0x1c,448);
    }
    void Call(uint32_t address,uint32_t a0=0,uint32_t a1=0,uint32_t a2=0,uint32_t entry=0) {
        CpuContext c{};
        c.cpuRegs.GPR.r[4].UL[0]=a0; c.cpuRegs.GPR.r[5].UL[0]=a1; c.cpuRegs.GPR.r[6].UL[0]=a2;
        bodies.at(address)(c,entry);
    }
    void Drain(bool expected=true) {
        auto tasks=std::move(pending); pending.clear();
        for (auto& task:tasks) { const bool ok=task.second(); if (expected) CHECK(ok); }
        scene.CollectAfterQuiescence();
    }
    Assets::View Load(uint32_t object,uint32_t id) {
        Put(object+4,id); Put(object+0x60,0xffffffff); Put(object+0x64,0);
        Put(TextureBindingFixture::stream+12,TextureBindingFixture::data);
        Assets::View result;
        CHECK(bindings.Read(object,TextureBindingFixture::stream,result)==title::TextureBindings::Result::Ok);
        return result;
    }
    Assets::View Sprite() {
        constexpr uint32_t shader=TextureBindingFixture::shader, material=0x150000;
        auto view=Load(TextureBindingFixture::object,0x046a8347);
        Put(shader,uint64_t{1}|uint64_t{4}|uint64_t{2}<<21|uint64_t{1}<<26|uint64_t{1}<<27|
                   uint64_t{1}<<28|uint64_t{1}<<55|uint64_t{1}<<59,8);
        Put(shader+0x14,view.token.object); Put(shader+0x18,uint16_t(-69),2);
        Put(shader+0x60,view.asset_id); Put(shader+0x64,13); Put(shader+0x6c,0x2f9ed8);
        Put(shader+0x58,0xffffffff); Put(shader+0x5c,0xffffffff);
        Put(material,shader); Put(material+0x10,1); Put(material+0x18,0x10000000);
        Put(material+0x20,24); Put(material+0x60,1);
        Put(sprite,material); Put(sprite+0xc,0x2f6b50);
        Float(sprite+0x10,0); Float(sprite+0x14,0); Float(sprite+0x18,1); Float(sprite+0x1c,1);
        return view;
    }
    void Font() {
        TextFixture source;
        title::TextLayout::Item item; item.text="abcdef"; item.colour=0x804080c0;
        source.SetItem(0,item);
        std::memcpy(ram.Data(),source.textures.ram.data(),source.textures.ram.size());
        constexpr uint32_t ids[]{0x922ed81e,0x96432c5c,0x9a57809a};
        constexpr uint32_t mats[]{0xb5cd358d,0xc1b8c44b,0xcda45309};
        for (uint32_t n=0;n<3;++n) {
            Load(TextureBindingFixture::object+n*0x100,ids[n]);
            Put(TextureBindingFixture::shader+n*0x100+0x60,ids[n]);
            Put(TextFixture::resources+n*0x10+4,mats[n]);
        }
        Display();
    }
};
using World = TestGraphicsProducers;
gs::DrawIr Triangle(const Scene::Frame& frame) {
    gs::DrawIr draw;
    draw.src_frame=frame.source_frame; draw.prim_type=3;
    draw.raster.frame={frame.color_buffer,frame.width,0,0};
    draw.raster.zbuf={frame.depth_buffer,0,false};
    draw.raster.test.zte=true; draw.raster.test.ztst=2;
    draw.raster.scissor={0,0,uint16_t(frame.width-1),uint16_t(frame.height-1)};
    draw.verts.resize(3); draw.vertex_count=3; draw.verts[1].x=16; draw.verts[2].y=16;
    return draw;
}
void SpriteOrderingAndLifetime() {
    Producers::Options options; options.sprites=true;
    World w(options);
    const auto original=std::vector<uint8_t>(w.vram.Bytes(),w.vram.Bytes()+gs::VramImage::kBytes);
    const auto texture=w.Sprite();
    CHECK(w.pipeline.DrawGeneric(Triangle(w.captured)).drew);
    const auto retained=w.backend.calls.back().params;
    w.Call(title::SpriteIntent::kColoured,World::sprite,0x80406080);
    CHECK(w.producers->Counts().sprites.replaced==1 && w.producers->Counts().sprites.model_calls==0);
    CHECK(w.backend.calls.size()==1 && w.backend.uploads==0);
    CHECK(w.pending.size()==1 && w.pending[0].first==24 && !w.producers->CanCheckpoint());
    // The accepted source record owns its complete frame/geometry/asset. A
    // changed guest object and released/reused allocation cannot redirect it.
    CHECK(w.assets.Release(texture.token)==Assets::Result::Ok);
    const auto fresh=w.Load(texture.token.object,texture.asset_id);
    CHECK(fresh.token.generation!=texture.token.generation);
    w.Put(World::sprite,0); w.Put(0x30a764,640);
    w.Drain();
    CHECK(w.producers->CanCheckpoint());
    const auto& delivered=w.backend.calls.back();
    CHECK(delivered.params.color.id==retained.color.id && delivered.params.depth.id==retained.depth.id);
    CHECK(!delivered.params.do_frame_clear && delivered.params.scale==2 && delivered.params.cd_copy==nullptr);
    CHECK(delivered.draw.raster.fba && delivered.draw.raster.alpha.a==2);
    CHECK(delivered.draw.raster.zbuf.zmsk && delivered.draw.raster.test.ztst==2);
    CHECK(delivered.draw.ctx==0 && !delivered.draw.fog && delivered.draw.tex.mmin==4);
    CHECK(delivered.texture.sampler.wms==1 && delivered.texture.sampler.wmt==1);
    CHECK(delivered.texture.transform.mip_k==-69 && delivered.texture.transform.mip_levels==4);
    CHECK(delivered.uploads==1 && w.backend.level_uploads==4 && w.backend.mip_finishes==1);
    CHECK(w.backend.At(delivered.texture.owner.surface).rgba==texture.image->levels[0].rgba);
    CHECK(w.pipeline.DrawGeneric(Triangle(w.captured)).drew);
    CHECK(w.backend.calls.back().params.color.id==retained.color.id && !w.backend.calls.back().params.do_frame_clear);
    CHECK(w.backend.readbacks==0 && w.backend.presents==0 && w.backend.clears==2);
    CHECK(std::memcmp(original.data(),w.vram.Bytes(),original.size())==0);
    CHECK(w.producers->Counts().recorded==1 && w.producers->Counts().completed==1);
}


// The untextured flat Sprite (TR materials.cpp MakeFlatMaterial: one
// type-0E shader, textured bit clear, no texture object; OLEG gives it to an icon
// slot with no picture) is selected without a texture and DRAWN at its slot --
// the first widened coldboot refused every such draw at the slot (detail
// 0x1220: an upload of the absent asset view), dropping it from both arms.
void FlatSpriteDrawsUntextured() {
    Producers::Options options; options.sprites=true;
    World w(options);
    constexpr uint32_t shader=TextureBindingFixture::shader, material=0x150000;
    w.Put(shader,uint64_t{1}|uint64_t{1}<<26|uint64_t{1}<<28,8); // blended, Gouraud, STQ, NOT textured
    w.Put(shader+0x14,0); w.Put(shader+0x60,0); w.Put(shader+0x64,14); w.Put(shader+0x6c,0x2f9db8);
    w.Put(shader+0x58,0xffffffff); w.Put(shader+0x5c,0xffffffff);
    w.Put(material,shader); w.Put(material+0x10,1); w.Put(material+0x18,2); w.Put(material+0x20,24);
    w.Put(World::sprite,material); w.Put(World::sprite+4,0); w.Put(World::sprite+0xc,0x2f6b50);
    w.Float(World::sprite+0x10,0); w.Float(World::sprite+0x14,0); w.Float(World::sprite+0x18,1); w.Float(World::sprite+0x1c,1);
    CHECK(w.pipeline.DrawGeneric(Triangle(w.captured)).drew);
    w.Call(title::SpriteIntent::kColoured,World::sprite,0x80406080);
    CHECK(w.producers->Counts().sprites.replaced==1 && w.producers->Counts().sprites.model_calls==0);
    CHECK(w.pending.size()==1 && w.pending[0].first==24);
    w.Drain();
    CHECK(w.producers->Counts().completed==1 && w.producers->Counts().refused_slots==0);
    CHECK(w.refusals.Of(title::GraphicsRefusals::Family::Sprites).total==0);
    const auto& delivered=w.backend.calls.back();
    CHECK(!delivered.draw.tex_enabled && delivered.draw.verts.size()==6 && delivered.uploads==0); // the strip's 2 triangles
    CHECK(delivered.draw.raster.alpha.a==0 && delivered.draw.raster.alpha.b==1 && delivered.draw.raster.alpha.d==1);
}

void EmptySpriteSourceClosure() {
    Producers::Options options; options.sprites = true;
    World w(options);
    const auto texture = w.Sprite();
    constexpr uint32_t matrix = 0x180000;
    std::memset(w.ram.Pointer(matrix,64),0,64);
    const auto before = std::vector<uint8_t>(w.ram.Data(),w.ram.Data()+w.ram.Size());
    const auto creates = w.backend.creates,uploads = w.backend.uploads,draws = w.backend.draws;
    w.Call(title::SpriteIntent::kPlacedColoured,World::sprite,matrix,0x80604020);
    const auto empty = w.producers->Counts();
    CHECK(empty.sprites.replaced == 1 && empty.sprites.empty == 1 &&
          !empty.sprites.model_calls && !empty.sprites.failed);
    CHECK(w.pending.empty() && !empty.recorded && !empty.completed && !empty.pending &&
          w.producers->CanCheckpoint());
    CHECK(w.backend.creates == creates && w.backend.uploads == uploads && w.backend.draws == draws);
    CHECK(std::memcmp(before.data(),w.ram.Data(),before.size()) == 0);
    for (unsigned lane : {0u,5u,10u,15u}) w.Float(matrix+lane*4,1);
    w.Call(title::SpriteIntent::kPlacedColoured,World::sprite,matrix,0x80604020);
    CHECK(w.pending.size() == 1 && !w.producers->Counts().completed);
    w.Drain();
    CHECK(w.producers->Counts().sprites.replaced == 2 && w.producers->Counts().sprites.empty == 1 &&
          w.producers->Counts().completed == 1 && w.backend.draws == draws+1 &&
          w.backend.uploads > uploads && w.producers->CanCheckpoint());
    Assets::View still_owned;
    CHECK(w.assets.Find(texture.lease,still_owned) == Assets::Result::Ok);
    app::GraphicsCensus census;
    census.Retire(w.producers->Counts(),w.bindings.Counts(),w.assets.Counts());
    CHECK(census.Producers({}).sprites.empty == 1 && census.Producers({}).sprites.replaced == 2);
    // No source environment, a mismatched source target and a texture this
    // owner never converted all REFUSE before any commit: the original sprite
    // draw runs (counted), nothing is queued, the owner stays checkpointable,
    // and the refusal is attributed with its family/reason.
    using Family = title::GraphicsRefusals::Family;
    using Reason = title::GraphicsRefusals::Reason;
    for (unsigned bad = 0; bad < 3; ++bad) {
        World other(options);
        other.Sprite();
        std::memset(other.ram.Pointer(matrix,64),0,64);
        if (bad == 0) other.source_ready = false;
        if (bad == 1) other.Put(title::MaterialState::kDepthBufferPage,167);
        if (bad == 2) CHECK(other.assets.Reset() == Assets::Result::Ok);
        other.Call(title::SpriteIntent::kPlacedColoured,World::sprite,matrix,0x80604020);
        CHECK(other.pending.empty() && !other.producers->Counts().sprites.empty &&
              !other.producers->Counts().sprites.replaced && other.producers->Counts().sprites.model_calls == 1);
        CHECK(other.model[title::SpriteIntent::kPlacedColoured] == 1 && !other.producers->Counts().sprites.failed);
        CHECK(other.producers->CanCheckpoint() && !other.backend.draws && !other.backend.uploads);
        const auto& row = other.refusals.Of(Family::Sprites);
        CHECK(row.total == 1 && row.faults == 0 && row.first.producer ==
              (bad == 2 ? uint32_t(TextureBindingFixture::object) : title::SpriteIntent::kPlacedColoured));
        CHECK(row.first.reason == (bad == 2 ? Reason::MissingResource : Reason::Ordering));
        CHECK(other.producers->Counts().sprites.refused == (bad == 2 ? 0u : 1u));
        CHECK(other.producers->Counts().refused_recordings == 0);
    }
}
void TextBatchesAndFlush() {
    Producers::Options options; options.text=true;
    World w(options); w.Font();
    w.Call(title::TextIntent::kDrawFontTexts,0,TextFixture::queue);
    w.Call(title::TextIntent::kDrawFontTexts,0,TextFixture::queue);
    CHECK(w.Word(World::count)==3 && w.producers->Counts().text_materials==3);
    CHECK(w.pending.empty() && !w.producers->CanCheckpoint());
    for (uint32_t n=0;n<3;++n) {
        CHECK(w.Word(World::list+n*4)==TextFixture::material+n*0x100);
        CHECK(w.Word(TextFixture::material+n*0x100+0x28)==0); // no writer packet
    }
    w.Call(Frame::kLinkAddress,0x13200); // foreign helper list doesn't finish this frame
    w.Call(Frame::kSetUpFrameAddress,World::target); // changes source generation: must not reinterpret old text
    Throws([&] { w.Call(0x1ba3a8,TextFixture::material); });
    CHECK(w.model[0x1ba3a8]==0 && w.pending.empty() && !w.producers->CanCheckpoint());

    World ready(options); ready.Font();
    ready.Put(TextFixture::resources+8,TextFixture::material|0x80000000u);
    ready.Call(title::TextIntent::kDrawFontTexts,0,TextFixture::queue);
    ready.Call(title::TextIntent::kDrawFontTexts,0,TextFixture::queue);
    for (uint32_t n=0;n<3;++n) ready.Call(0x1ba3a8,TextFixture::material+n*0x100);
    CHECK(ready.pending.size()==3 && ready.backend.calls.empty());
    CHECK(ready.producers->Counts().flush_replaced==3 && ready.producers->Counts().flush_model_calls==0);
    CHECK(ready.producers->Counts().text.replaced==2 && ready.producers->Counts().text.model_calls==0);
    CHECK(ready.Word(World::list)==(TextFixture::material|0x80000000u));
    ready.Call(0x1c0410);
    CHECK(ready.Word(World::count)==0 && ready.producers->Counts().text_materials==0);
    CHECK(!ready.producers->CanCheckpoint());
    ready.Drain();
    CHECK(ready.producers->CanCheckpoint() && ready.backend.calls.size()==6);
    const auto color=ready.backend.calls.front().params.color.id;
    for (size_t n=0;n<ready.backend.calls.size();++n) {
        const auto& call=ready.backend.calls[n];
        CHECK(call.draw.use_fst && call.draw.tex.fst && call.texture.sampler.wms==1);
        CHECK(call.draw.raster.frame.fbmsk==0xff000000 && call.draw.raster.zbuf.zmsk);
        CHECK(call.params.color.id==color && call.params.do_frame_clear==(n==0));
        CHECK(call.texture.transform.mip_k==-69 && call.texture.transform.mip_levels==4);
    }
    CHECK(ready.backend.uploads==3 && ready.backend.presents==0 && ready.backend.readbacks==0);
}
void SelectionAndFailures() {
    World disabled;
    disabled.Call(title::SpriteIntent::kPlain,0,0,0,title::SpriteIntent::kPlain+4);
    CHECK(disabled.producers->Counts().sprites.model_calls==1 && disabled.model[title::SpriteIntent::kPlain]==1);
    CHECK(!disabled.producers->MovieOutput().record);
    for (unsigned variant=0;variant<6;++variant) {
        Producers::Options options; options.sprites=true;
        World w(options); const auto asset=w.Sprite();
        if (variant==0) w.source_ready=false;
        if (variant==1) w.accept=false;
        if (variant==2) CHECK(w.assets.Release(asset.token)==Assets::Result::Ok);
        if (variant==3) w.Put(TextureBindingFixture::shader+0x64,3);
        if (variant==4) w.backend.fail_draw=true;
        if (variant==5) w.late_refusal=true;
        // 0/1/2/5 refuse BEFORE any commit (no environment, reservation refused,
        // texture never converted, reservation refused with a retained closure):
        // the original draws and the owner stays live. 3 (an owned texture in
        // an unclassified shader) and 4 (a failed delivery) are faults: 4 is the
        // renderer failing at the ordered slot -- the delivery THROWS (the
        // ordered output's walk faults), it is never a silent false.
        const bool refuses = variant==0 || variant==1 || variant==2 || variant==5;
        if (variant==3) Throws([&] { w.Call(title::SpriteIntent::kColoured,World::sprite,0x80808080); });
        else if (variant==4) {
            w.Call(title::SpriteIntent::kColoured,World::sprite,0x80808080);
            Throws([&] { w.pending.front().second(); }); w.pending.clear();
            CHECK(w.producers->Counts().failures==1 && w.producers->Counts().refused_slots==0 &&
                  w.producers->Counts().pending==0);
        } else w.Call(title::SpriteIntent::kColoured,World::sprite,0x80808080);
        CHECK(w.model[title::SpriteIntent::kColoured]==(refuses ? 1u:0u) &&
              w.producers->Counts().sprites.model_calls==(refuses ? 1u:0u));
        CHECK(w.producers->CanCheckpoint()==refuses);
        if (refuses) {
            w.Call(title::SpriteIntent::kColoured,World::sprite,0x80808080);
            CHECK(w.model[title::SpriteIntent::kColoured]==2 && !w.producers->Counts().sprites.failed);
            CHECK(w.refusals.Of(title::GraphicsRefusals::Family::Sprites).total==2);
        } else {
            Throws([&] { w.Call(title::SpriteIntent::kColoured,World::sprite,0x80808080); });
            CHECK(w.producers->Counts().sprites.failed>=1 || w.producers->Counts().failures>=1);
        }
        if (variant==5) {
            // The transport kept both closures: their cancelled tickets return
            // false at delivery without touching the owner or replaying a draw.
            CHECK(w.pending.size()==2 && w.producers->Counts().pending==0 &&
                  w.producers->Counts().refused_recordings==2);
            w.Drain(false);
            CHECK(w.producers->Counts().pending==0 && w.backend.calls.empty());
        }
    }
    for (unsigned variant=0;variant<4;++variant) {
        Producers::Options options; options.text=true;
        World w(options); w.Font();
        if (variant==0) {
            // A material already listed by a retained writer: RecordText refuses
            // before publishing any list word -> the original DrawFontTexts runs.
            w.Put(TextFixture::material+0x64,1,1);
            w.Call(title::TextIntent::kDrawFontTexts,0,TextFixture::queue);
            CHECK(w.Word(World::count)==0 && w.producers->Counts().text_materials==0);
            CHECK(w.model[title::TextIntent::kDrawFontTexts]==1 && w.producers->Counts().text.refused==1 &&
                  w.producers->Counts().text.model_calls==1 && w.producers->CanCheckpoint());
            CHECK(w.refusals.Of(title::GraphicsRefusals::Family::Text).first.detail==title::TextService::kRecordRefused);
            continue;
        } else {
            w.Call(title::TextIntent::kDrawFontTexts,0,TextFixture::queue);
            if (variant==1) Throws([&] { w.Call(Frame::kLinkAddress,Frame::kFrameBuckets); });
            if (variant==2) {
                w.Put(TextureBindingFixture::shader+0x18,27,2);
                Throws([&] { w.Call(0x1ba3a8,TextFixture::material); });
            }
            if (variant==3) Throws([&] { w.Call(0x1c0410); });
        }
        CHECK(w.model[0x1ba3a8]==0 && w.model[0x1c0410]==0 && !w.producers->CanCheckpoint());
    }
}

// Textures at load with a UI producer deselected (the player's `--set
// hle.graphics_text=off` / `graphics_sprites=off` over the all-on default):
// the selection converts every record (Texture() is true for the font page
// and the HUD picture alike), yet the producers install -- the agreement is
// on the services, not on the convert policy -- and the deselected consumer
// stays the original's (a counted model call), the selected one draws.
void TexturesAtLoadWithUiDeselected() {
    for (unsigned variant=0;variant<3;++variant) {
        Producers::Options options; options.textures=true;
        options.sprites = variant!=1; options.text = variant!=0 && variant!=2;
        World w(options);   // the fixture CHECKs Install
        CHECK(w.selection.Texture(0,0x922ed81e) && w.selection.Texture(0,0x046a8347));
        CHECK(w.selection.Ready().textures && w.selection.Ready().text==options.text &&
              w.selection.Ready().sprites==options.sprites);
        if (variant!=1) {
            w.Sprite();
            w.Call(title::SpriteIntent::kColoured,World::sprite,0x80808080);
            CHECK(w.model[title::SpriteIntent::kColoured]==0 && w.producers->Counts().sprites.model_calls==0);
            w.Drain();
        } else {
            w.Call(title::SpriteIntent::kPlain,0,0,0,title::SpriteIntent::kPlain+4);
            CHECK(w.model[title::SpriteIntent::kPlain]==1 && w.producers->Counts().sprites.model_calls==1);
        }
    }
}

void CancelUnsubmitted() {
    Producers::Options options; options.sprites = true;
    World w(options);
    w.Sprite();
    w.Call(title::SpriteIntent::kPlain, World::sprite);
    const auto before = w.producers->Counts();
    CHECK(before.recorded == 1 && before.pending == 1 && before.completed == 0);
    // Source execution has stopped and the output owner discards an unlinked
    // group. No draw, upload, model retry or completion credit is manufactured.
    w.pending.clear();
    const auto after = w.producers->Counts();
    CHECK(after.pending == 0 && after.cancelled == 1 && after.completed == 0);
    CHECK(after.recorded == 1 && after.failures == 0 && w.backend.calls.empty());
    CHECK(w.producers->CanCheckpoint());
}
void MovieViewAndSnapshot() {
    Producers::Options options; options.movie=true;
    World w(options);
    auto output=w.producers->MovieOutput();
    gs::DrawIr draw;
    CHECK(Movie::BuildDraw({512,448,512,448,false,false},draw));
    auto raw=std::make_shared<Movie::Image>();
    raw->id=1; raw->pixels.width=512; raw->pixels.height=448;
    raw->pixels.rgba.resize(512*448*4);
    for (size_t n=0;n<raw->pixels.rgba.size();++n) raw->pixels.rgba[n]=uint8_t(n/2048);
    auto view=std::make_shared<Movie::Image>();
    view->id=2; view->source_id=1;
    CHECK(Movie::BuildImageView(draw,raw->pixels,view->pixels));
    for (const auto& v:draw.verts) {
        view->sample_width=std::max(view->sample_width,(uint32_t(v.uv_u)+7)/16);
        view->sample_height=std::max(view->sample_height,(uint32_t(v.uv_v)+7)/16);
    }
    CHECK(output.upload(raw) && output.record(draw,view));
    CHECK(w.pending.size()==2 && w.pending[0].first==UINT32_MAX && w.pending[1].first==27);
    w.Put(0x30a764,640); w.frame.ResetAfterQuiescence(); // delivery must not recapture
    CHECK(!output.record(draw,view));
    w.Drain();
    CHECK(w.backend.calls.size()==1 && w.backend.uploads==2);
    const auto& call=w.backend.calls.back();
    CHECK(w.backend.At(call.texture.owner.surface).rgba==view->pixels.rgba);
    const auto& pixels=w.backend.At(call.texture.owner.surface).rgba;
    const size_t guard=(size_t(view->pixels.height-1)*view->pixels.width+view->pixels.width-1)*4;
    const size_t edge=(size_t(view->sample_height-1)*raw->pixels.width+view->sample_width-1)*4;
    CHECK(std::memcmp(pixels.data()+guard,raw->pixels.rgba.data()+edge,4)==0);
    CHECK(call.texture.transform.uv_tex_w==view->pixels.width && call.texture.transform.uv_tex_h==view->pixels.height);
    CHECK(call.draw.ctx==1 && call.draw.use_fst && call.draw.raster.frame.fbmsk==0xff000000);
    CHECK(call.draw.raster.zbuf.zmsk && call.draw.raster.test.ztst==2);
    CHECK(call.params.color.id==w.cache.Find(0x70000)->views.color.id && call.params.scale==2);
    CHECK(w.backend.presents==0 && w.backend.readbacks==0 && w.backend.clears==2);
    CHECK(w.producers->Counts().recorded==2 && w.producers->Counts().completed==2);
}
void SkyOwnedAttachments() {
    World w;
    title::SkyPass::Output sky;
    sky.setup.width=512; sky.setup.height=448;
    title::MaterialState::Environment environment; environment.colour_clamp=true;
    CHECK(title::SkyIntent::Build(512,448,environment,sky.finish)==title::SkyIntent::Result::Ok);
    auto texture=std::make_shared<title::SkyTexture::Image>();
    texture->width=texture->height=128; texture->rgba.resize(128*128*4,0x80);
    auto draw=Triangle(w.captured);
    draw.raster.frame.fbw=256; draw.raster.frame.fbp=0x230000;
    draw.raster.test.ztst=1;
    draw.tex_enabled=true; draw.tex.psm=19; draw.tex.tw=draw.tex.th=7;
    draw.tex.tcc=1; draw.tex.tfx=1; draw.tex.k=-69;
    for (auto& v:draw.verts) v.q=1;
    sky.draws.push_back({draw,texture});
    CHECK(w.draws.Sky(w.captured,sky));
    CHECK(w.backend.calls.size()==2);
    const auto& geometry=w.backend.calls[0];
    const auto& finish=w.backend.calls[1];
    CHECK(geometry.params.render_w==256 && geometry.params.render_h==224 && geometry.params.scale==2);
    CHECK(!geometry.params.do_frame_clear && geometry.texture.transform.mip_k==-69);
    CHECK(finish.texture.owner.surface.id==geometry.params.color.id);
    CHECK(finish.params.color.id==w.cache.Find(0x70000)->views.color.id);
    CHECK(geometry.params.color.id!=finish.params.color.id && finish.params.do_frame_clear);
    const auto sky_color=geometry.params.color.id;
    CHECK(w.draws.Sky(w.captured,sky));
    CHECK(w.backend.calls[2].params.color.id==sky_color && !w.backend.calls[3].params.do_frame_clear);
    CHECK(w.backend.uploads==1 && w.backend.presents==0 && w.backend.readbacks==0 && w.backend.clears==3);
    CHECK(sky_color==w.cache.Find(0x230000)->views.color.id);
    // A preceding retained pass, including its unwritten pixels, is the same
    // allocation sampled by an empty selected Sky pass. No copy/extra clear.
    w.backend.SetPixel({sky_color},17,19,31,47,63,79);
    sky.draws.clear();
    CHECK(w.draws.Sky(w.captured,sky));
    CHECK(w.backend.At(w.backend.calls.back().texture.owner.surface).rgba[(19*512+17)*4]==31);
    CHECK(w.backend.clears==3 && w.backend.uploads==1);
}
void SkyProducerComposition() {
    Producers::Options options; options.sky=true; options.movie=true;
    World w(options);
    constexpr uint32_t sky=0x10000, view=0x12000;
    w.Put(title::SkySetup::kRenderView,view);
    const title::SkyGeometry::Matrix identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    std::memcpy(w.ram.Pointer(view+0x80,sizeof(identity)),&identity,sizeof(identity));
    // Source calls StartSkyBuffer even with no models. Preserve the inherited
    // half-size image through the complete producer composition.
    auto& inherited=w.cache.GetOrCreate(0x230000,256,224,0,0);
    w.backend.SetPixel(inherited.views.color,17,19,31,47,63,79);
    inherited.coherence.has_content = true;
    inherited.coherence.valid_rect.Union(0,0,256,224);
    w.Call(Frame::kLinkAddress,Frame::kFrameBuckets);
    w.Call(title::SkySetup::kAddress,sky);
    CHECK(w.producers->Counts().sky.start.replaced==1 && !w.producers->CanCheckpoint());
    Scene::Frame before;
    CHECK(!w.frame.CaptureScene(27,before));
    gs::DrawIr movie;
    CHECK(Movie::BuildDraw({512,448,512,448,false,false},movie));
    CHECK(!w.producers->MovieOutput().record(movie,{})); // unfinished source frame
    w.Call(title::SkyIntent::kAddress,sky);
    CHECK(w.producers->Counts().sky.finish.replaced==1 && w.pending.size()==1);
    CHECK(w.pending[0].first==0 && w.frame.CaptureScene(27,before));
    CHECK(before.source_frame==1 && before.epoch==w.captured.epoch+1);
    w.frame.ResetAfterQuiescence(); // immutable source frame travels with the work
    w.Drain();
    CHECK(w.backend.calls.size()==1 && w.backend.calls.back().draw.src_frame==1);
    const auto& sampled=w.backend.At(w.backend.calls.back().texture.owner.surface);
    CHECK(sampled.rgba[(19*512+17)*4]==31);
    CHECK(w.backend.calls.back().texture.owner.surface.id==inherited.views.color.id);
    CHECK(w.model[title::SkySetup::kAddress]==0 && w.model[title::SkyIntent::kAddress]==0);
    CHECK(w.producers->Counts().sky.start.model_calls==0 && w.producers->Counts().sky.finish.model_calls==0);
    CHECK(w.producers->CanCheckpoint());
    // Unknown source environment cannot select: the StartSkyBuffer REFUSES
    // (counted, attributed Ordering/Environment) and the original runs; the
    // pass continues as a model pass until the source FinishSky.
    w.Call(title::SkySetup::kAddress,sky);
    CHECK(w.model[title::SkySetup::kAddress]==1 && w.producers->Counts().sky.start.model_calls==1 &&
          w.producers->Counts().sky.start.replaced==1 && w.producers->Counts().sky.refused==1);
    CHECK(w.producers->Counts().sky.last_refusal==title::SkyPass::Refusal::Environment);
    CHECK(w.refusals.Of(title::GraphicsRefusals::Family::Sky).total==1 &&
          w.refusals.Of(title::GraphicsRefusals::Family::Sky).first.reason==title::GraphicsRefusals::Reason::Ordering);
    w.Call(title::SkyIntent::kAddress,sky);
    CHECK(w.model[title::SkyIntent::kAddress]==1 && w.producers->Counts().sky.finish.replaced==1);
    CHECK(w.pending.empty() && w.producers->CanCheckpoint());
}

void EmptySkyRequiresPreservedContent() {
    title::SkyPass::Output sky;
    sky.setup.width = 512; sky.setup.height = 448;
    title::MaterialState::Environment environment; environment.colour_clamp = true;
    CHECK(title::SkyIntent::Build(512,448,environment,sky.finish) == title::SkyIntent::Result::Ok);
    World w;
    CHECK(!w.draws.Sky(w.captured,sky));
    CHECK(w.backend.creates == 0 && w.backend.calls.empty());
    auto& inherited = w.cache.GetOrCreate(0x230000,256,224,0,0);
    const int allocations = w.backend.creates, clears = w.backend.clears;
    CHECK(!w.draws.Sky(w.captured,sky)); // allocated but unauthored is not valid input
    CHECK(w.backend.creates == allocations && w.backend.clears == clears && w.backend.calls.empty());
    inherited.coherence.has_content = true;
    inherited.coherence.valid_rect.Union(0,0,256,224);
    w.backend.SetPixel(inherited.views.color,17,19,31,47,63,79);
    title::SceneState::Snapshot saved;
    CHECK(w.scene.CaptureState(saved));
    World restored;
    CHECK(restored.scene.StageState(saved) && restored.scene.PublishState());
    const auto restored_image = restored.cache.Find(0x230000)->views.color;
    CHECK(restored.draws.Sky(restored.captured,sky));
    CHECK(restored.backend.calls.size() == 1);
    const auto& sampled = restored.backend.calls.back().texture.owner.surface;
    CHECK(sampled.id == restored_image.id);
    CHECK(restored.backend.At(sampled).rgba[(19*512+17)*4] == 31);
    CHECK(restored.backend.At(sampled).rgba[(19*512+17)*4+3] == 79);
    CHECK(restored.backend.presents == 0);
    restored.cache.AliasPages().MarkAliased(0x230000,8192);
    CHECK(!restored.draws.Sky(restored.captured,sky));
    CHECK(restored.backend.calls.size() == 1);
}

void EmptySkyComposedRefusal() {
    Producers::Options options; options.sky = true;
    World w(options);
    constexpr uint32_t sky = 0x10000, view = 0x12000;
    w.Put(title::SkySetup::kRenderView,view);
    const title::SkyGeometry::Matrix identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    std::memcpy(w.ram.Pointer(view+0x80,sizeof(identity)),&identity,sizeof(identity));
    w.Call(title::SkySetup::kAddress,sky);
    w.Call(title::SkyIntent::kAddress,sky);
    CHECK(w.pending.size() == 1 && w.pending[0].first == 0);
    CHECK(!w.pending[0].second());
    w.pending.clear(); // consumed exactly once, despite the refused selected output
    // The owned sky declined at its ordered slot (no preserved content to
    // compose): a REFUSAL of the slot -- counted, attributed to the Sky family
    // (ordering, kSlotRefused, the FinishSky producer), nothing drawn, the
    // run not failed and the owner still checkpointable; never a failure.
    CHECK(w.producers->Counts().completed == 0 && w.producers->Counts().failures == 0 &&
          w.producers->Counts().refused_slots == 1);
    CHECK(w.producers->Counts().pending == 0 && w.producers->CanCheckpoint());
    const auto& sky_refusals = w.refusals.Of(title::GraphicsRefusals::Family::Sky);
    CHECK(sky_refusals.total == 1 && sky_refusals.last.reason == title::GraphicsRefusals::Reason::Ordering &&
          sky_refusals.last.producer == title::SkyPass::kDrawModel && sky_refusals.last.detail == 0x200);
    CHECK(w.model[title::SkySetup::kAddress] == 0 && w.model[title::SkyIntent::kAddress] == 0);
    CHECK(w.backend.calls.empty() && w.backend.creates == 0);
}
}
int main() {
    CancelUnsubmitted();
    SpriteOrderingAndLifetime();
    FlatSpriteDrawsUntextured();
    EmptySpriteSourceClosure();
    TextBatchesAndFlush();
    SelectionAndFailures();
    TexturesAtLoadWithUiDeselected();
    MovieViewAndSnapshot();
    SkyOwnedAttachments();
    SkyProducerComposition();
    EmptySkyRequiresPreservedContent();
    EmptySkyComposedRefusal();
    std::printf("GraphicsProducers: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
