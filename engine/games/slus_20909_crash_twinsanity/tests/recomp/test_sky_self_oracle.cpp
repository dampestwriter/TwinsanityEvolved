#include "games/slus_20909_crash_twinsanity/tests/recomp/sky_oracle_memory.h"

#include <algorithm>
#include <cfenv>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <stdexcept>

#include "engine/core/hash/hash.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/state/gs_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_renderer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_service.h"
#include "tests/lib/fake_backend.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace sky_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
}
namespace {
using Draw = ps2r::gs::DrawIr;
using Sky = title::SkyIntent;
using Service = title::SkyService;
using Graphics = title::Graphics;
using Targets = title::GraphicsTargets;
using Resources = title::GraphicsResources;
using Renderer = title::GraphicsRenderer;
using Environment = title::MaterialState::Environment;
namespace ram = sky_oracle::memory;
constexpr uint32_t kBucket = 0x323658, kChain = 0x323300;
constexpr uint32_t kAnchor = 0x11000, kPacket = 0x12000;
size_t cases = 0, compared_vertices = 0, inactive_differences = 0;
std::ofstream comparison;
[[noreturn]] void Fail(const char* expression, int line) {
    std::fprintf(stderr, "FAIL case %zu line %d: %s\n", cases, line, expression);
    std::exit(1);
}
#define REQUIRE(value) do { if (!(value)) Fail(#value, __LINE__); } while (false)

auto Reader() {
    return [](uint32_t address, void* data, size_t size) {
        if (address > ram::bytes.size() || size > ram::bytes.size() - address) return false;
        std::memcpy(data, ram::bytes.data() + address, size);
        return true;
    };
}
CpuContext Context() {
    CpuContext result{};
    result.cpuRegs.GPR.r[29].UL[0] = 0x2f0000;
    // Source never reads its Sky*. A poison pointer catches accidental reliance.
    result.cpuRegs.GPR.r[4].UL[0] = 0xffffffff;
    return result;
}
void Fixture(uint32_t width, uint32_t height) {
    std::memset(ram::bytes.data(), 0, ram::bytes.size());
    ram::write(title::MaterialState::kDisplayWidth, width);
    ram::write(title::MaterialState::kDisplayHeight, height);
    ram::write<uint8_t>(0x30a498, 0xab); // generated stack-only read, no draw effect
    ram::write(kBucket + 0x10, kAnchor);
    ram::write<uint32_t>(kBucket + 0x18, 0);
    ram::write<uint32_t>(kAnchor, 0x20000000);
    ram::write(kChain + 0x10, kPacket);
}

struct Sink : ps2r::gs::DrawIrSink {
    std::vector<Draw> draws;
    void OnDraw(const Draw& draw) override { draws.push_back(draw); }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};

Draw Run(bool selected, const Environment& environment) {
    auto snapshot = std::make_unique<decltype(ram::bytes)>();
    std::memcpy(snapshot->data(), ram::bytes.data(), ram::bytes.size());
    ps2r::test::FakeBackend backend;
    Resources resources(backend);
    Graphics queue;
    Targets targets(queue);
    Renderer renderer(backend, resources);
    const uint32_t width = ram::read<uint32_t>(title::MaterialState::kDisplayWidth);
    const uint32_t height = ram::read<uint32_t>(title::MaterialState::kDisplayHeight);
    using Format = ps2r::render::SurfaceFormat;
    Graphics::Handle color, depth, copy, sky;
    REQUIRE(resources.Register(queue, {width,height,Format::Rgba8,true,1}, &color) == Resources::Result::Ok);
    REQUIRE(resources.Register(queue, {width,height,Format::Depth32f,false,1}, &depth) == Resources::Result::Ok);
    REQUIRE(resources.Register(queue, {width,height,Format::Rgba8,true,1}, &copy) == Resources::Result::Ok);
    REQUIRE(resources.Register(queue, {width/2,height/2,Format::Rgba8,true,1}, &sky) == Resources::Result::Ok);
    REQUIRE(targets.Bind(Targets::Name::SceneColor, color) == Targets::Result::Ok);
    REQUIRE(targets.Bind(Targets::Name::SceneDepth, depth) == Targets::Result::Ok);
    REQUIRE(targets.Bind(Targets::Name::SceneCopy, copy) == Targets::Result::Ok);
    REQUIRE(targets.Bind(Targets::Name::SkyColor, sky) == Targets::Result::Ok);
    REQUIRE(queue.BeginFrame(0) == Graphics::Result::Ok);
    // Explicit owned input pixels. Their producer is outside this slice; this
    // oracle does not pretend the still-open sky geometry has been retired.
    const std::vector<uint8_t> pixels(size_t(width/2) * (height/2) * 4, 0x78);
    REQUIRE(queue.RecordUpload(queue.End(0), sky, {0,0,int(width/2),int(height/2)},
                              (width/2)*4, pixels) == Graphics::Result::Ok);
    size_t model_calls = 0;
    Service::Body dispatch = [&](CpuContext& context, uint32_t entry) {
        ++model_calls;
        sky_oracle::FUN_001b9f30(context, entry);
    };
    Service::Hooks hooks;
    hooks.enabled = selected;
    hooks.read = Reader();
    hooks.environment = [&](Environment& out) { out = environment; return true; };
    hooks.record = [&](const Sky::Output& intent) {
        REQUIRE(intent.width == width && intent.height == height);
        return targets.Record(queue.End(Sky::kBucket), intent.draw, Targets::Name::SkyColor) == Targets::Result::Ok;
    };
    Service service(std::move(hooks));
    REQUIRE(service.Install([&](uint32_t entry) {
        REQUIRE(entry == Sky::kAddress); return dispatch;
    }, [&](uint32_t entry, Service::Body body) {
        REQUIRE(entry == Sky::kAddress); dispatch = std::move(body); return true;
    }));
    auto context = Context();
    const auto input_context = context;
    dispatch(context, Sky::kAddress);
    Sink result;
    if (selected) {
        REQUIRE(service.Counts().replaced == 1 && service.Counts().model_calls == 0 && model_calls == 0);
        REQUIRE(std::memcmp(snapshot->data(), ram::bytes.data(), ram::bytes.size()) == 0);
        REQUIRE(std::memcmp(&input_context, &context, sizeof(context)) == 0);
        Graphics::Hooks delivery;
        delivery.upload = [&](const Graphics::Upload& upload) { return resources.Upload(upload) == Resources::Result::Ok; };
        delivery.draw = [&](const Draw& draw, const std::vector<Graphics::Lease>& bindings) {
            result.draws.push_back(draw);
            ps2r::render::SurfaceId texture;
            REQUIRE(resources.Resolve(bindings.at(Targets::Texture), &texture) == Resources::Result::Ok);
            REQUIRE(backend.At(texture).rgba == pixels);
            return renderer.Draw(draw, bindings) == Renderer::Result::Ok;
        };
        delivery.drain = [] { return true; };
        REQUIRE(queue.Submit(delivery) == Graphics::Result::Ok);
        REQUIRE(backend.draws == 1 && backend.uploads == 1 && backend.readbacks == 0);
        REQUIRE(backend.last_params.depth.id != 0 && !backend.last_params.do_frame_clear);
        REQUIRE(queue.Counts().replaced_bucket_jobs[0] == 2); // one upload and one draw
    } else {
        REQUIRE(service.Counts().replaced == 0 && service.Counts().model_calls == 1 && model_calls == 1);
        REQUIRE(ram::read<uint32_t>(kAnchor + 4) == kPacket);
        auto model = std::make_unique<ps2r::gs::GsState>();
        model->SetSink(&result);
        model->WriteRegister(0x22, environment.scan_mask);
        model->WriteRegister(0x49, environment.per_pixel_alpha);
        model->WriteRegister(0x44, environment.dither_matrix);
        model->WriteRegister(0x3d, uint64_t(environment.fog_r) |
            uint64_t(environment.fog_g) << 8 | uint64_t(environment.fog_b) << 16);
        // Seed inactive state with nonzero values to prove the exact scope of
        // canonicalization. Active alpha/depth/texture state is never ignored.
        model->WriteRegister(0x43, 0x7f00000044ull);
        model->WriteRegister(0x35, 0x123456789abcdef);
        model->WriteRegister(0x37, 0xfedcba987654321);
        GsStateSink sink(*model);
        GifRegs regs;
        GifParser parser(sink, regs);
        const uint32_t qwords = ram::read<uint32_t>(kPacket) & 0xffff;
        REQUIRE(qwords == (width/32)*4 + 18);
        parser.ReceiveData(GifPath::Path2, ram::bytes.data() + kPacket + 16, qwords * 16);
        REQUIRE(parser.CanAcceptDirect());
        REQUIRE(sink.Kicks() == 2 * (width/32));
        REQUIRE(queue.Reset() == Graphics::Result::Ok);
    }
    REQUIRE(result.draws.size() == 1);
    return result.draws.at(0);
}

void Compare(Draw model, const Draw& host, uint32_t width, uint32_t height, int rounding) {
    REQUIRE(!model.alpha_blend && !host.alpha_blend && model.tex.mxl == 0 && host.tex.mxl == 0);
    const auto raw = model.ToDrawRec();
    // FinishSky disables blending and mips. Old context-1 ALPHA and MIPTBP
    // values do not enter this intent and are the ONLY normalized fields.
    model.raster.alpha = {};
    for (size_t i = 0; i != 6; ++i) model.tex.mip_tbp[i] = model.tex.mip_tbw[i] = 0;
    const auto a = model.ToDrawRec(), b = host.ToDrawRec();
    inactive_differences += std::memcmp(&raw, &a, sizeof(a)) != 0;
    if (std::memcmp(&a, &b, sizeof(a))) {
        for (size_t i = 0; i < sizeof(a); ++i) {
            const auto x = reinterpret_cast<const uint8_t*>(&a)[i];
            const auto y = reinterpret_cast<const uint8_t*>(&b)[i];
            if (x != y) std::fprintf(stderr, "DrawRec byte %zu: %02x/%02x\n", i, x, y);
        }
        Fail("complete active DrawRec equality", __LINE__);
    }
    REQUIRE(model.raster.dthe == host.raster.dthe && model.raster.dimx == host.raster.dimx);
    REQUIRE(model.tex.mxl == host.tex.mxl && model.tex.lcm == host.tex.lcm);
    REQUIRE(model.tex.l == host.tex.l && model.tex.k == host.tex.k);
    REQUIRE(std::equal(std::begin(model.tex.mip_tbp), std::end(model.tex.mip_tbp), std::begin(host.tex.mip_tbp)));
    REQUIRE(std::equal(std::begin(model.tex.mip_tbw), std::end(model.tex.mip_tbw), std::begin(host.tex.mip_tbw)));
    REQUIRE(model.verts.size() == host.verts.size());
    REQUIRE(std::memcmp(model.verts.data(), host.verts.data(), host.verts.size()*sizeof(host.verts[0])) == 0);
    REQUIRE(host.raster.test.zte && host.raster.test.ztst == 1 && !host.raster.zbuf.zmsk);
    REQUIRE(!host.raster.fba && !host.raster.dthe && host.raster.colclamp);
    REQUIRE(host.raster.scissor.x1 == 1024 && host.raster.scissor.y1 == 1024);
    REQUIRE(host.bbox[3] == float((width/32)*32));
    for (const auto& v : host.verts) REQUIRE(v.z == 0 && v.a == 0);
    compared_vertices += host.verts.size();
    if (comparison) {
        comparison << width << '\t' << height << '\t' << rounding << '\t' << std::hex
            << ps2r::hash::Fnv1a64(&raw, sizeof(raw)) << '\t'
            << ps2r::hash::Fnv1a64(&a, sizeof(a)) << '\t'
            << ps2r::hash::Fnv1a64(&b, sizeof(b)) << '\t'
            << model.snapshot.vertex_hash << '\t' << host.snapshot.vertex_hash
            << std::dec << "\t0\t0\n";
    }
}

void TestDeclinesAndInstallation() {
    Fixture(512, 448);
    Environment environment;
    Sky::Output unchanged;
    unchanged.width = 99;
    REQUIRE(Sky::Capture(Reader(), 0x1ba198, environment, unchanged) == Sky::Result::UnsupportedEntry);
    REQUIRE(Sky::Capture({}, 0, environment, unchanged) == Sky::Result::InvalidMemory);
    REQUIRE(Sky::Capture([](uint32_t, void*, size_t) -> bool { throw 1; }, 0, environment, unchanged) == Sky::Result::InvalidMemory);
    for (auto dimensions : {std::pair{0u,448u}, {127u,448u}, {1025u,224u},
                            {512u,0u}, {512u,1u}, {512u,1025u}, {1024u,1024u},
                            {0xffffffffu,0xffffffffu}}) {
        REQUIRE(Sky::Build(dimensions.first, dimensions.second, environment, unchanged) == Sky::Result::UnsupportedFrame);
        REQUIRE(unchanged.width == 99);
    }
    environment.scan_mask = 4;
    REQUIRE(Sky::Build(512,448,environment,unchanged) == Sky::Result::UnsupportedFrame);
    size_t originals = 0, recordings = 0;
    bool ready = false, accept = false, throw_record = false;
    Service::Body dispatch = [&](CpuContext&, uint32_t) { ++originals; };
    Service::Hooks hooks;
    hooks.enabled = true;
    hooks.read = Reader();
    hooks.environment = [&](Environment& out) { out = {}; return ready; };
    hooks.record = [&](const Sky::Output&) {
        ++recordings;
        if (throw_record) throw std::runtime_error("record failed");
        return accept;
    };
    Service service(hooks);
    REQUIRE(!service.Install([](uint32_t) { return Service::Body{}; },
                             [](uint32_t, Service::Body) { Fail("unexpected installer", __LINE__); return true; }));
    REQUIRE(!service.Install([&](uint32_t) { return dispatch; }, [](uint32_t, Service::Body) { return false; }));
    REQUIRE(service.Install([&](uint32_t) { return dispatch; },
                            [&](uint32_t, Service::Body body) { dispatch = std::move(body); return true; }));
    auto context = Context();
    dispatch(context, Sky::kAddress); // missing source environment
    REQUIRE(originals == 1 && recordings == 0);
    ready = true;
    dispatch(context, 0); // recorder declines before side effects
    REQUIRE(originals == 2 && recordings == 1);
    accept = true;
    dispatch(context, 0x1ba198); // restored interior owns generated stack
    REQUIRE(originals == 3 && recordings == 1 && service.Counts().interior_calls == 1);
    dispatch(context, Sky::kAddress);
    REQUIRE(service.Counts().replaced == 1 && originals == 3);
    throw_record = true;
    bool caught = false;
    try { dispatch(context, Sky::kAddress); } catch (const std::runtime_error&) { caught = true; }
    REQUIRE(caught && originals == 3 && service.Counts().recording_failures == 1);
    caught = false;
    try { dispatch(context, Sky::kAddress); } catch (const std::logic_error&) { caught = true; }
    REQUIRE(caught && originals == 3);
}

void TestFrameEnvironmentComposition() {
    Fixture(512, 448);
    size_t original = 0, replacement = 0;
    std::map<uint32_t, Service::Body> dispatch;
    Service::Hooks hooks;
    hooks.enabled = true;
    hooks.read = Reader();
    hooks.environment = [](Environment& out) { out = {}; return true; };
    hooks.record = [&](const Sky::Output&) { ++replacement; return true; };
    Service service(hooks);
    const auto capture = [&](uint32_t address) {
        auto it = dispatch.find(address);
        return it != dispatch.end() ? it->second : Service::Body([&](CpuContext&, uint32_t) { ++original; });
    };
    const auto install = [&](uint32_t address, Service::Body body) { dispatch[address] = std::move(body); return true; };
    REQUIRE(service.Install(capture, install));
    title::FrameEnvironment frame;
    REQUIRE(frame.Install(capture, install)); // observation wraps either arm
    auto context = Context();
    dispatch.at(Sky::kAddress)(context, Sky::kAddress);
    Environment output;
    REQUIRE(frame.Read(output));
    REQUIRE(frame.Observed().completed_frame_boundaries == 1 && original == 0 && replacement == 1);
    dispatch.at(Sky::kAddress)(context, 0x1ba234);
    REQUIRE(!frame.Read(output) && original == 1);
}
}  // namespace

int main(int argc, char** argv) {
    REQUIRE(!Service::Hooks{}.enabled);
    if (argc > 1) {
        comparison.open(argv[1]);
        REQUIRE(bool(comparison));
        comparison << "width\theight\trounding\tmodel_raw\tmodel_active\thost_active\tmodel_vertices\thost_vertices\tactive_differences\tselected_model_calls\n";
    }
    TestDeclinesAndInstallation();
    TestFrameEnvironmentComposition();
    const int original_rounding = std::fegetround();
    const int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    for (uint32_t width = 128; width <= 1024; ++width) {
        const uint32_t limit = std::min(1024u, uint32_t((4*1024*1024)/11) / width);
        const uint32_t height = width % 7 == 0 ? limit : 2 + (width * 71) % (limit - 1);
        Environment environment;
        environment.scan_mask = width % 4;
        environment.per_pixel_alpha = width % 2;
        environment.dither_matrix = 0x6071243571603524ull ^ width;
        environment.fog_r = uint8_t(width); environment.fog_g = 0x80; environment.fog_b = 0x78;
        Fixture(width, height);
        const int rounding = modes[width % 4];
        // As in StripSelfOracle, generated execution/assembly uses the NEW
        // guest-thread contract. The host producer must not depend on its
        // caller inheriting that rounding mode (including signed-zero math).
        REQUIRE(std::fesetround(FE_TOWARDZERO) == 0);
        auto model = Run(false, environment);
        Fixture(width, height);
        REQUIRE(std::fesetround(rounding) == 0);
        auto host = Run(true, environment);
        REQUIRE(std::fegetround() == rounding);
        Compare(std::move(model), host, width, height, rounding);
        ++cases;
    }
    REQUIRE(std::fesetround(original_rounding) == 0);
    std::printf("SkySelfOracle: %zu generated-body cases, %zu compared vertices, "
                "0 active DrawIR/vertex differences, 0 selected model calls, "
                "%zu inactive-state differences\n", cases, compared_vertices, inactive_differences);
    return 0;
}
