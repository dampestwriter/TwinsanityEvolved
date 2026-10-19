#include "games/slus_20909_crash_twinsanity/tests/recomp/display_oracle_memory.h"

#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <cstring>

#include "engine/core/hash/hash.h"
#include "engine/gs/hle/queue/draw_ir_queue_consumer.h"
#include "engine/gs/state/gs_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_service.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"
#include "tests/lib/fake_backend.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.h"

namespace display_oracle::memory {
std::array<uint8_t,0x400000> bytes{};
std::function<void(uint32_t,uint64_t)> privileged;
}
namespace {
using namespace ps2r;
using Intent = title::DisplayLayout;
using Service = title::FrameService;
using Scene = title::GraphicsScene;
namespace ram = display_oracle::memory;
size_t checks = 0,failures = 0,cases = 0;
retirement::OracleArtifacts artifacts;
uint64_t layout_model_calls[2] = {0,0}, layout_replaced[2] = {0,0}, layouts_applied[2] = {0,0};
template <class T> void Word(std::vector<uint8_t>& out, T value) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL case%zu line%d: %s\n",cases,__LINE__,#x); } } while(false)
class TestDisplayLayoutSelfOracle {
public:
    test::FakeBackend backend;
    title::GraphicsSurfaces surfaces{backend};
    gs::VramImage memory;
    gs::VramLayout layout;
    gs::ScalePolicy scale{gs::ScaleSettings{2},&layout};
    gs::RenderTargetCache cache{surfaces,memory,scale};
    gs::TargetRefresher refresher{surfaces,memory,cache};
    gs::TextureResolver resolver{surfaces,memory,cache,refresher,layout};
    gs::ScanoutComposer scanout{surfaces,cache};
    gs::Presenter presenter{scanout};
    gs::SequenceRecipe sequence{gs::SequenceRecipe::Constants{}};
    gs::DrawPipeline pipeline{gs::DrawPipeline::Deps{
        &surfaces,&memory,&cache,&refresher,&resolver,&scanout,&presenter,&layout,nullptr,&sequence}};
    title::GraphicsResources resources{surfaces};
    title::GraphicsRenderer renderer{surfaces,resources};
    Scene scene{surfaces,resources,renderer,pipeline};
    unsigned resizes = 0,generated = 0,other = 0;
    std::vector<std::pair<uint32_t,uint64_t>> writes;
    std::map<uint32_t,Service::Body> dispatch;
    gs::DrawIrQueueConsumer consumer;
    gs::GsState model;
    Service service;
    explicit TestDisplayLayoutSelfOracle(bool selected)
        : consumer(pipeline,ConsumerHooks()),service({selected,Reader(),
            [](auto&) { return false; },[](auto,const auto&) { return false; },{},{},
            [this](const auto& out) {
                const bool resize = pipeline.WindowW() != int(out.width) ||
                    pipeline.WindowH() != int(out.height);
                if (scene.ConfigureDisplay(out) != Scene::Result::Ok) return false;
                if (resize) ++resizes;
                return true;
            }}) {
        // Same inherited host layout in both executions, deliberately unlike
        // the source image. Real consumer applies generated model jobs.
        pipeline.OnSetWindow(711,599,123,45);
        pipeline.OnSetFrontBuffer(0x220000,704,0);
        model.SetSink(&consumer);
        CHECK(service.Install([this](auto at) -> Service::Body {
            if (at == Intent::kAddress) return [this](auto& ctx,auto pc) {
                ++generated; display_oracle::FUN_0019aff0(ctx,pc);
            };
            return [this](auto&,auto) { ++other; };
        },[this](auto at,auto body) { dispatch[at] = std::move(body); return true; }));
    }
    static title::MaterialState::Read Reader() {
        return [](uint32_t at,void* out,size_t n) {
            if (at > ram::bytes.size() || n > ram::bytes.size()-at) return false;
            std::memcpy(out,ram::bytes.data()+at,n); return true;
        };
    }
    gs::DrawIrQueueConsumer::Hooks ConsumerHooks() {
        gs::DrawIrQueueConsumer::Hooks hooks;
        hooks.window_resized = [this](int,int) { ++resizes; };
        return hooks;
    }
    void Run(uint32_t width,uint32_t height) {
        CHECK(service.RestorePlan(width,height));
        const auto before = hash::Fnv1a64(ram::bytes.data(),ram::bytes.size());
        ram::privileged = [this](auto at,auto value) {
            writes.emplace_back(at,value); model.WritePrivileged(at,value);
        };
        CpuContext ctx{};
        dispatch.at(Intent::kAddress)(ctx,Intent::kAddress);
        ram::privileged = {};
        CHECK(before == hash::Fnv1a64(ram::bytes.data(),ram::bytes.size()));
        CHECK(!other && !backend.draws && !backend.creates && !backend.uploads &&
              !backend.readbacks && !backend.clears && !backend.presents);
        CHECK(!presenter.EeFrameCount() && !presenter.FrameCount());
    }
};
using World = TestDisplayLayoutSelfOracle;

void Seed(uint32_t w,uint32_t h,uint32_t mag) {
    std::memset(ram::bytes.data(),0,ram::bytes.size());
    // Both arms start from the same source settings, including ignored words.
    std::array<uint32_t,22> s{0,w/64,2,0x33333333,0x44444444,0x55555555,0x66666666,
        128,1,1,1,0,0,0,0xeeeeeeee,0xffffffff,mag,0,(mag+1)*w,h-1,0,0};
    std::memcpy(ram::bytes.data()+Intent::kSettings,s.data(),sizeof(s));
    ram::write(title::MaterialState::kDisplayWidth,w);
    ram::write(title::MaterialState::kDisplayHeight,h);
}
bool Equal(const title::SceneState::Viewport& a,const title::SceneState::Viewport& b) {
    return a.width == b.width && a.height == b.height && a.content_width == b.content_width &&
        a.content_height == b.content_height && a.color_key == b.color_key &&
        a.buffer_width == b.buffer_width && a.format == b.format;
}
void Compare(uint32_t w,uint32_t h,uint32_t mag,std::ofstream& evidence) {
    auto model = std::make_unique<World>(false),host = std::make_unique<World>(true);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        const uint32_t width = repeat == 2 ? 512 : w,height = repeat == 2 ? 448 : h;
        const uint32_t magnify = repeat == 2 ? 4 : mag;
        Seed(width,height,magnify); model->Run(width,height);
        const auto input = hash::Fnv1a64(ram::bytes.data(),ram::bytes.size());
        Seed(width,height,magnify); host->Run(width,height);
        const auto a = model->scene.CaptureViewport(),b = host->scene.CaptureViewport();
        CHECK(Equal(a,b));
        if (artifacts.Enabled()) {
            // Consumed input on both arms: the 22 source settings words and the display globals
            // (Seed writes nothing else). Output: the published host viewport and resize count.
            for (bool native : {false,true}) {
                auto& in = artifacts.Input(native);
                Word(in, uint32_t(0x59414c44));
                in.insert(in.end(), ram::bytes.data()+Intent::kSettings, ram::bytes.data()+Intent::kSettings+22*4);
                Word(in, width); Word(in, height);
                const auto& v = native ? b : a;
                auto& out = artifacts.Output(native, "viewport");
                Word(out, v.width); Word(out, v.height); Word(out, v.content_width); Word(out, v.content_height);
                Word(out, v.color_key); Word(out, v.buffer_width); Word(out, v.format);
                Word(out, uint32_t(native ? host->resizes : model->resizes));
            }
        }
        CHECK(b.width == width && b.height == height && b.content_width == width &&
              b.content_height == height && b.color_key == 0 && b.buffer_width == width && b.format == 2);
        CHECK(model->resizes == host->resizes);
        CHECK(model->generated == repeat+1 && model->writes.size() == (repeat+1)*6);
        CHECK(!host->generated && host->writes.empty() && host->model.Priv().Writes() == 0);
        CHECK(model->service.Counts().layout.model_calls == repeat+1 &&
              !model->service.Counts().layout.replaced);
        CHECK(host->service.Counts().layout.replaced == repeat+1 &&
              !host->service.Counts().layout.model_calls && host->scene.DisplayLayouts() == repeat+1);
        if (repeat == 2) {  // the owners' census after their three runs
            for (bool native : {false,true}) {
                const auto& c = (native ? host : model)->service.Counts().layout;
                layout_model_calls[native] += c.model_calls; layout_replaced[native] += c.replaced;
                layouts_applied[native] += (native ? host : model)->scene.DisplayLayouts();
            }
        }
        const auto base = repeat*6;
        const std::array<uint32_t,6> offsets{0,0x20,0x80,0xa0,0x70,0x90};
        for (unsigned i = 0; i < offsets.size(); ++i)
            CHECK(model->writes.at(base+i).first == 0x12000000+offsets[i]);
        CHECK(model->writes.at(base).second == 0x8023);
        CHECK(model->writes.at(base+1).second == 1);
        CHECK(model->writes.at(base+4).second == (0x10000u | (width/64)<<9));
        if (evidence) {
            evidence << width << '\t' << height << '\t' << magnify << '\t' << repeat
                << '\t' << std::hex << input;
            for (unsigned i = 0; i < 6; ++i) evidence << '\t' << model->writes.at(base+i).second;
            evidence << std::dec << '\t' << a.width << 'x' << a.height << '\t'
                << b.width << 'x' << b.height << '\t' << host->resizes << "\t0\t0\t"
                << Equal(a,b) << '\n';
        }
        ++cases;
    }
    // A fresh owner resumes with the exact published host layout. No register
    // inference is allowed on restore; an identical next setup needs no resize.
    title::SceneState::Snapshot snapshot;
    std::string why;
    CHECK(host->scene.CaptureState(snapshot,&why));
    auto restored = std::make_unique<World>(true);
    CHECK(restored->scene.StageState(snapshot,&why));
    CHECK(restored->scene.PublishState(&why));
    Seed(512,448,4); restored->Run(512,448);
    CHECK(!restored->resizes && Equal(restored->scene.CaptureViewport(),host->scene.CaptureViewport()));
    CHECK(restored->scene.DisplayLayouts() == 1 && !restored->generated);
}

void Refusals() {
    Intent::Output out{99,88};
    CHECK(Intent::Capture({},0,out) == Intent::Result::InvalidMemory && out.width == 99);
    CHECK(Intent::Capture(World::Reader(),Intent::kAddress+4,out) == Intent::Result::UnsupportedEntry);
    for (const auto index : {0u,1u,2u,7u,8u,9u,10u,11u,12u,13u,16u,17u,18u,19u,20u,21u}) {
        Seed(512,448,4);
        const auto at = Intent::kSettings+index*4;
        ram::write(at,ram::read<uint32_t>(at)^1u);
        CHECK(Intent::Capture(World::Reader(),0,out) == Intent::Result::UnsupportedLayout &&
              out.width == 99 && out.height == 88);
    }
    for (const auto size : {Intent::Output{0,448},Intent::Output{513,448},
                           Intent::Output{1024,1024},Intent::Output{512,0}}) {
        Seed(size.width,size.height,4);
        CHECK(Intent::Capture(World::Reader(),0,out) == Intent::Result::UnsupportedLayout);
    }
}
} // namespace

int main(int argc,char** argv) {
    std::ofstream evidence;
    // [evidence.tsv] [--export-dir DIR --source-manifest FILE]
    const bool tsv = argc >= 2 && std::strncmp(argv[1], "--", 2) != 0;
    CHECK(artifacts.Parse(argc, argv, tsv ? 2 : 1));
    if (tsv) {
        evidence.open(argv[1]); CHECK(bool(evidence));
        evidence << "width\theight\tmagnify\trepeat\tinput\tpmode\tsmode2\tdisplay1\tdisplay2"
                    "\tdispfb1\tdispfb2\tmodel_view\thost_view\tresizes\tdraws\tvertices\tequal\n";
    }
    for (auto wm : {std::pair{64u,15u},std::pair{256u,9u},std::pair{320u,7u},
                    std::pair{384u,6u},std::pair{512u,4u},std::pair{640u,3u}})
        for (uint32_t height : {1u,224u,448u,512u})
            Compare(wm.first,height,wm.second,evidence);
    Refusals();
    std::printf("DisplayLayoutSelfOracle: %zu cases, zero draw/vertex output, "
                "%zu checks, %zu failures\n",cases,checks,failures);
    if (!failures && artifacts.Enabled()) {
        for (bool native : {false,true}) {
            const auto row = [&](uint64_t calls, uint64_t replaced, uint64_t model_calls) {
                return "calls=" + std::to_string(calls) + ",replaced=" + std::to_string(replaced) +
                       ",model_calls=" + std::to_string(model_calls); };
            artifacts.Counter(native, "graphics_frame_setup", row(0,0,0));
            artifacts.Counter(native, "graphics_frame_send", row(0,0,0));
            artifacts.Counter(native, "graphics_frame_interrupt", row(0,0,0));
            artifacts.Counter(native, "graphics_frame_clear", row(0,0,0));
            artifacts.Counter(native, "graphics_frame_layout", row(layout_model_calls[native]+layout_replaced[native],
                layout_replaced[native], layout_model_calls[native]));
            artifacts.Counter(native, "graphics_frame_other", "foreign_sends=0,interior_calls=0,failed=0");
            artifacts.Counter(native, "graphics_frame_output", "completed=0,setups=0,layouts=" + std::to_string(layouts_applied[native]));
        }
        artifacts.Setting("hle.graphics_frame", "off", "on");
        artifacts.Note("scope", "source SetupPCRTC display layout: the published host viewport (destination, content, "
            "front-target key/width/format) and resize count after each setup; the model arm reaches it through the "
            "generated body's privileged register writes applied by the real consumer; zero draw/vertex output");
        artifacts.Note("canonicalization", "none");
        artifacts.Note("counter_provenance", "graphics_frame_layout: FrameService::Run census, published by "
            "hle/graphics/graphics_report.cpp; only the layout boundary is exercised here; graphics_frame_output.layouts "
            "= GraphicsScene::DisplayLayouts() (model arm: zero, its layout reaches the scene through the consumer)");
        artifacts.Finish("DisplayLayoutSelfOracle", "display-layout-artifacts-v1", argc, argv);
    }
    return failures ? 1 : 0;
}
