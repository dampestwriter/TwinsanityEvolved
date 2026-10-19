#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

#include <algorithm>
#include <cfenv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <vector>

#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/state/gs_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_service.h"
#include "games/slus_20909_crash_twinsanity/composition/graphics_strip.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_export.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_scene_fixture.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
std::map<uint32_t, size_t> calls;
void ObserveGenerated(uint32_t address) { ++calls[address]; }
void ExternalCall(uint32_t address, CpuContext& context) {
    // The source shader's allocation is the sole substituted game operation.
    // Its actual generated setup writes every register in the returned buffer.
    if (address == 0x1815c0 && context.cpuRegs.GPR.r[4].UL[0] <= 0x100) {
        context.cpuRegs.GPR.r[2].UD[0] = 0x15000;
        return;
    }
    std::fprintf(stderr, "unhandled generated callee %08x\n", address);
    std::abort();
}
}  // namespace strip_oracle

namespace {
using Service = title::GraphicsService;
using Material = title::MaterialState;
using Draw = ps2r::gs::DrawIr;
namespace ram = strip_oracle::memory;
constexpr uint32_t kShape = 0x1000, kPositions = 0x2000, kColors = 0x3000;
constexpr uint32_t kShader = 0x4000, kTarget = 0x6000, kMatrix = 0x7000;
constexpr uint32_t kRoot = 0x10000, kAnchor = 0x11000, kGeometry = 0x12000;
constexpr uint32_t kTargetSetup = 0x16000;
constexpr uint32_t kBuckets = title::GraphicsBridge::kFrameBucketsAddress;
size_t cases = 0, vertices = 0, inactive_fst_differences = 0;
size_t shared_scene_draws = 0;
std::ofstream comparison;
strip_oracle::Export artifacts;
bool export_run = true;
uint64_t rng = 0x915da55eaebba34ull;

[[noreturn]] void Fail(const char* expression, int line) {
    std::fprintf(stderr, "FAIL case %zu line %d: %s\n", cases, line, expression);
    std::exit(1);
}
#define REQUIRE(value) do { if (!(value)) Fail(#value, __LINE__); } while (false)
uint32_t Next() {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return uint32_t(rng);
}
CpuContext Context() {
    CpuContext context{};
    context.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    context.cpuRegs.GPR.r[29].UL[0] = 0x2f0000;
    context.vuRegs.VF[0].w = 1;
    return context;
}
struct Sink : ps2r::gs::DrawIrSink {
    std::vector<Draw> draws;
    void OnDraw(const Draw& draw) override { draws.push_back(draw); }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};

Material::Environment Input(size_t test, size_t count, bool repeated) {
    std::memset(ram::bytes.data(), 0, ram::bytes.size());
    ram::write(kShape, Material::kFlatMaterial);
    ram::write<uint8_t>(kShape + 0x10, uint8_t(count));
    ram::write(kShape + 0x14, kPositions);
    ram::write(kShape + 0x18, kColors);
    ram::write(Material::kFlatMaterial, kShader);
    ram::write<uint32_t>(Material::kFlatMaterial + 0x10, 1);
    ram::write<uint64_t>(Material::kFlatMaterial + 0x18, 2);
    ram::write<uint32_t>(Material::kFlatMaterial + 0x20, 24);
    // Source flat settings, with inactive test parameters and FST varied.
    const uint64_t settings = 0x0800000004400001ull |
        (uint64_t(Next() & 0x1fff) << 6) | (uint64_t(test & 1) << 20) |
        (uint64_t((test >> 1) & 1) << 28);
    ram::write(kShader, settings);
    ram::write<uint32_t>(kShader + 0x64, 14);
    ram::write(kShader + 0x6c, Material::kFlatShaderVtable);
    // Exact type E slots: ShaderRegisters and ShaderType0ERegistersSize.
    // NEW pal_ntsc_map.tsv rows 3521/3737; the generated JALRs execute both.
    ram::write<uint32_t>(Material::kFlatShaderVtable + 0x3c, 0x1d88d8);
    ram::write<uint32_t>(Material::kFlatShaderVtable + 0x24, 0x1dbb38);
    ram::write<uint64_t>(0x3d6b20, 0x44); // source alpha preset: Mix
    const uint32_t width = 256 + uint32_t(test % 3) * 128;
    const uint32_t height = 224 + uint32_t(test % 2) * 224;
    ram::write(Material::kDisplayWidth, width);
    ram::write(Material::kDisplayHeight, height);
    ram::write<uint32_t>(Material::kDisplayPixelBytes, 2);
    ram::write<uint32_t>(Material::kDepthBufferPage, 168 + test % 4);
    ram::write(Material::kRenderTarget, kTarget);
    ram::write<uint32_t>(kTarget + 4, test % 17);
    ram::write<uint32_t>(kTarget + 8, test % 11);
    ram::write(kTarget + 0xc, width); ram::write(kTarget + 0x10, height);
    // Coordinate dimensions deliberately differ from the frame's XYOFFSET.
    ram::write<uint32_t>(kTarget + 0x18, width + test % 13);
    ram::write<uint32_t>(kTarget + 0x1c, height + test % 7);
    ram::write(kTarget + 0x68, kTargetSetup);
    ram::write(kBuckets + 8, kRoot);
    ram::write(kBuckets + 24 * 0x38 + 0x10, kAnchor);
    ram::write<uint32_t>(kBuckets + 24 * 0x38 + 0x18, 0);
    ram::write<uint32_t>(kBuckets + 24 * 0x38 + 0x2c,
                         repeated ? Material::kFlatMaterial : 0);
    ram::write<uint32_t>(kAnchor, 0x20000000);
    ram::write<uint32_t>(0x323300 + 0x10, kGeometry);
    for (size_t i = 0; i < count; ++i) {
        float x = float(int32_t(Next() % 16000001) - 8000000) / 1024;
        float y = float(int32_t(Next() % 16000001) - 8000000) / 1024;
        if (test % 7 == 0 && i > 2 && i % 3 == 0) {
            x = ram::read<float>(kPositions + uint32_t(i - 1) * 8);
            y = ram::read<float>(kPositions + uint32_t(i - 1) * 8 + 4);
        }
        ram::write(kPositions + uint32_t(i) * 8, x);
        ram::write(kPositions + uint32_t(i) * 8 + 4, y);
        ram::write(kColors + uint32_t(i) * 4, Next());
    }
    for (size_t i = 0; i < 16; ++i)
        ram::write(kMatrix + uint32_t(i) * 4,
                   float(int32_t(Next() % 131073) - 65536) / 8192);
    Material::Environment environment;
    environment.colour_clamp = test & 1;
    environment.per_pixel_alpha = (test >> 1) & 1;
    environment.scan_mask = test % 4;
    environment.dither = (test >> 2) & 1;
    environment.dither_matrix = uint64_t(Next()) | (uint64_t(Next()) << 32);
    environment.fog_r = uint8_t(Next());
    environment.fog_g = uint8_t(Next());
    environment.fog_b = uint8_t(Next());
    return environment;
}

void SetupGenerated() {
    auto context = Context();
    context.cpuRegs.GPR.r[4].UL[0] = kShader;
    strip_oracle::RunGenerated(0x1d3f38, context);
    context = Context();
    context.cpuRegs.GPR.r[4].UL[0] = kTarget;
    strip_oracle::RunGenerated(0x19ef90, context);
    REQUIRE(ram::read<uint32_t>(kShader + 0x58) == 0x15000);
    REQUIRE(ram::read<uint32_t>(kShader + 0x5c) == 7);
}

std::vector<Draw> Run(bool selected, size_t variant, uint32_t tint,
                      const Material::Environment& environment, bool collapsed = false) {
    Sink output;
    std::vector<std::function<void()>> slots;
    std::map<uint32_t, Service::Body> bindings;
    for (uint32_t address : Service::kStripAddresses)
        bindings[address] = [address](CpuContext& context, uint32_t) {
            strip_oracle::RunGenerated(address, context);
        };
    bindings[Service::kLinkAddress] = [](CpuContext&, uint32_t) {};
    Service::Hooks hooks;
    hooks.strip_selection = selected ? Service::StripSelection::All : Service::StripSelection::Model;
    hooks.read = [](uint32_t address, uint8_t* bytes, size_t size) {
        if (address > ram::bytes.size() || size > ram::bytes.size() - address) return false;
        std::memcpy(bytes, ram::bytes.data() + address, size);
        return true;
    };
    hooks.environment = [&](Material::Environment& out) { out = environment; return true; };
    hooks.flush_staged = [] {};
    hooks.delivery.enqueue = [&](std::function<void()> fn) {
        slots.push_back(std::move(fn)); return true;
    };
    hooks.delivery.sink = &output;
    hooks.delivery.flush_legacy = [] { return true; };
    hooks.delivery.drain = [] { return true; };
    Service* active = nullptr;
    std::unique_ptr<ps2r::test::StripSceneFixture> scene;
    const uint32_t width = ram::read<uint32_t>(Material::kDisplayWidth);
    const uint32_t height = ram::read<uint32_t>(Material::kDisplayHeight);
    const title::GraphicsScene::Frame frame{
        width*height*2/8192*8192,ram::read<uint32_t>(Material::kDepthBufferPage)*8192,
        width,height,0,1};
    ps2r::app::GraphicsStrip owned([&](auto& out) { out = frame; return true; },
        [&](uint32_t bucket,auto work) {
            return active && active->RecordOwned(bucket,[&,work=std::move(work)] {
                scene = std::make_unique<ps2r::test::StripSceneFixture>(frame);
                ps2r::app::GraphicsStrip::Retained retained;
                const bool ok = work(scene->scene,retained) == ps2r::app::GraphicsStrip::Outcome::Completed;
                if (ok) ++shared_scene_draws;
                return ok;
            });
        });
    hooks.record_strip = [&](auto intent) {
        output.OnDraw(intent.draw); // exact source DrawIR, also consumed below
        return owned.Submit(std::move(intent));
    };
    Service service(std::move(hooks));
    REQUIRE(service.Install(
        [&](uint32_t address) { return bindings.at(address); },
        [&](uint32_t address, Service::Body body) {
            bindings[address] = std::move(body); return true;
        }));
    active = &service;
    auto context = Context();
    context.cpuRegs.GPR.r[4].UL[0] = kShape;
    context.cpuRegs.GPR.r[5].UL[0] = variant >= 2 ? kMatrix : tint;
    context.cpuRegs.GPR.r[6].UL[0] = tint;
    const auto input = std::make_unique<decltype(ram::bytes)>(ram::bytes);
    const uint32_t packet_start = ram::read<uint32_t>(0x323300 + 0x10);
    const bool empty = ram::read<uint8_t>(kShape+0x10) < 3 || collapsed;
    // Artifacts: the caller-contract rounding mode only, on both arms,
    // for single-call cases (the continuation sequences' second model call
    // consumes the model's own packet writes, so their images differ by design).
    const bool recorded = export_run && std::fegetround() == FE_TOWARDZERO;
    if (recorded)
        artifacts.Input(selected, variant, {context.cpuRegs.GPR.r[4].UL[0], context.cpuRegs.GPR.r[5].UL[0],
                        context.cpuRegs.GPR.r[6].UL[0]}, environment, input->data(), input->size());
    strip_oracle::calls.clear();
    bindings.at(Service::kStripAddresses[variant])(context, Service::kStripAddresses[variant]);
    if (selected) {
        REQUIRE(service.Counts().model_producers == 0);
        REQUIRE(service.Counts().replaced_producers == 1);
        REQUIRE(strip_oracle::calls.empty());
        REQUIRE(ram::bytes == *input); // no producer/setup/packet/cache writes
        context.cpuRegs.GPR.r[4].UL[0] = kBuckets;
        bindings.at(Service::kLinkAddress)(context, Service::kLinkAddress);
        service.OnTag(kRoot, false, true);
        service.OnTag(kAnchor, false, false);
        service.EndWalk();
        for (auto& slot : slots) slot();
        REQUIRE(service.CanCheckpoint());
        if (empty)
            REQUIRE(service.Counts().empty_producers == 1 &&
                    service.Counts().output.completed == 0 && slots.empty() && !scene);
        else {
            REQUIRE(scene && owned.Counts().captured == 1 && owned.Counts().completed == 1 &&
                    !owned.Counts().failures && !service.Counts().output.completed &&
                    service.Counts().output.completed_work == 1 &&
                    service.Counts().owned_strip_producers == 1);
            REQUIRE(scene->backend.draws == 1 && !scene->backend.texture.enabled &&
                    !scene->backend.presents && !scene->backend.uploads && !scene->backend.readbacks);
            REQUIRE(scene->pipeline.DrawSeq() == 1 && scene->renderer.Counts().draws == 1);
            REQUIRE(scene->backend.last.raster.frame.fbp == 0 && scene->backend.last.raster.zbuf.zbp == 0);
            const auto& sent = output.draws.back();
            const auto& received = scene->backend.last;
            REQUIRE(sent.verts.size() == received.verts.size() &&
                    !std::memcmp(sent.verts.data(),received.verts.data(),
                                 sent.verts.size()*sizeof(ps2r::gs::VertexRec)));
            REQUIRE(received.raster.fba && received.raster.alpha.b == 1 &&
                    received.raster.test.ztst == 2 && received.raster.zbuf.zmsk);
        }
    } else {
        REQUIRE(service.Counts().model_producers == 1);
        REQUIRE(service.Counts().replaced_producers == 0);
        REQUIRE(strip_oracle::calls[Service::kStripAddresses[variant]] == 1);
        REQUIRE(strip_oracle::calls[0x1a6068] == 1);
        REQUIRE(strip_oracle::calls[0x1a6190] == 1);
        const bool repeated = (*input)[kBuckets + 24 * 0x38 + 0x2c] != 0;
        REQUIRE(strip_oracle::calls[0x1a0640] == (repeated ? 0u : 1u));
        REQUIRE(strip_oracle::calls[0x1c0278] == (repeated ? 0u : 1u));
        REQUIRE(strip_oracle::calls[0x1d88d8] == (repeated ? 0u : 1u));
        REQUIRE(strip_oracle::calls[0x1dbb38] == (repeated ? 0u : 2u));
        REQUIRE(ram::bytes != *input);
        auto model = std::make_unique<ps2r::gs::GsState>();
        model->SetSink(&output);
        // Only shared source frame environment is supplied directly. Shader,
        // target, color and geometry all come from the actual generated body.
        model->WriteRegister(0x1a, 0); // source shared Begin2D uses PRMODE
        model->WriteRegister(0x46, environment.colour_clamp);
        model->WriteRegister(0x49, environment.per_pixel_alpha);
        model->WriteRegister(0x22, environment.scan_mask);
        model->WriteRegister(0x45, environment.dither);
        model->WriteRegister(0x44, environment.dither_matrix);
        model->WriteRegister(0x3d, uint64_t(environment.fog_r) |
            (uint64_t(environment.fog_g) << 8) | (uint64_t(environment.fog_b) << 16));
        GsStateSink sink(*model);
        GifRegs regs;
        GifParser parser(sink, regs);
        parser.ReceiveData(GifPath::Path2, ram::bytes.data() + kTargetSetup + 16, 64);
        parser.ReceiveData(GifPath::Path2, ram::bytes.data() + 0x15000, 7 * 16);
        const uint32_t geometry = ram::read<uint32_t>(0x30a7a0);
        REQUIRE(geometry == packet_start + (repeated ? 0u : 32u));
        if (!repeated) {
            REQUIRE(ram::read<uint32_t>(packet_start) == 0x50000000);
            REQUIRE(ram::read<uint32_t>(packet_start + 4) == kTargetSetup);
            REQUIRE(ram::read<uint32_t>(packet_start + 16) == 0x30000007);
            REQUIRE(ram::read<uint32_t>(packet_start + 20) == 0x15000);
        }
        const uint32_t qwords = ram::read<uint32_t>(geometry) & 0xffff;
        REQUIRE(qwords == 1u + 2u * ram::read<uint8_t>(kShape + 0x10));
        parser.ReceiveData(GifPath::Path2, ram::bytes.data() + geometry + 16, qwords * 16);
        REQUIRE(parser.CanAcceptDirect());
        REQUIRE(sink.Kicks() == ram::read<uint8_t>(kShape + 0x10));
    }
    REQUIRE(output.draws.size() == (empty ? 0u : 1u));
    if (recorded) {
        artifacts.Output(selected, output.draws.empty() ? nullptr : &output.draws.front());
        artifacts.Counters(selected, service.Counts(), owned.Counts());
    }
    return output.draws;
}

void Compare(Draw model, const Draw& draw, size_t variant, int rounding) {
    REQUIRE(!model.tex_enabled && !draw.tex_enabled && !model.fog && !draw.fog);
    // DrawFlush records the LAST UV/ST register write even for untextured
    // geometry. The source intent has no such dependency. Report, then
    // canonicalize ONLY that inactive flag, never active material or geometry.
    const bool inactive_fst_difference = model.tex.fst != draw.tex.fst;
    inactive_fst_differences += inactive_fst_difference;
    const auto raw = model.ToDrawRec();
    model.tex.fst = draw.tex.fst;
    auto a = model.ToDrawRec(), b = draw.ToDrawRec();
    if (std::memcmp(&a, &b, sizeof(a))) {
        const auto* x = reinterpret_cast<const uint8_t*>(&a);
        const auto* y = reinterpret_cast<const uint8_t*>(&b);
        for (size_t i = 0; i < sizeof(a); ++i)
            if (x[i] != y[i])
                std::fprintf(stderr, "variant %zu rounding %d DrawRec byte %zu: %02x/%02x\n",
                             variant, rounding, i, x[i], y[i]);
        Fail("complete DrawRec equality", __LINE__);
    }
    // These fields are not in the DrawRec serialization.
    REQUIRE(model.raster.dthe == draw.raster.dthe);
    REQUIRE(model.raster.dimx == draw.raster.dimx);
    REQUIRE(model.tex.mxl == draw.tex.mxl && model.tex.lcm == draw.tex.lcm);
    REQUIRE(model.tex.l == draw.tex.l && model.tex.k == draw.tex.k);
    REQUIRE(std::equal(std::begin(model.tex.mip_tbp), std::end(model.tex.mip_tbp),
                       std::begin(draw.tex.mip_tbp)));
    REQUIRE(std::equal(std::begin(model.tex.mip_tbw), std::end(model.tex.mip_tbw),
                       std::begin(draw.tex.mip_tbw)));
    REQUIRE(model.verts.size() == draw.verts.size());
    REQUIRE(std::memcmp(model.verts.data(), draw.verts.data(),
                        draw.verts.size() * sizeof(ps2r::gs::VertexRec)) == 0);
    // OLD native compatibility pins: FBA stays post-blend, exact Mix,
    // GEQUAL at source front depth, no depth writes, and no inherited DATE.
    REQUIRE(draw.raster.fba && draw.raster.alpha.a == 0 && draw.raster.alpha.b == 1 &&
            draw.raster.alpha.c == 0 && draw.raster.alpha.d == 1);
    REQUIRE(draw.raster.test.zte && draw.raster.test.ztst == 2 &&
            draw.raster.zbuf.zmsk && !draw.raster.test.date);
    for (const auto& vertex : draw.verts) {
        uint32_t z;
        std::memcpy(&z, &vertex.z, sizeof(z));
        REQUIRE(z == 0x4f7fffff);
    }
    if (comparison.is_open()) {
        using Format = ps2r::gs::GsdlFormat;
        comparison << cases << '\t' << variant << '\t' << rounding << '\t'
            << draw.verts.size() << '\t' << std::hex
            << Format::Fnv1a(&raw, sizeof(raw)) << '\t'
            << Format::Fnv1a(&a, sizeof(a)) << '\t'
            << Format::Fnv1a(&b, sizeof(b)) << '\t'
            << model.snapshot.vertex_hash << '\t' << draw.snapshot.vertex_hash
            << std::dec << '\t' << inactive_fst_difference << "\t0\t0\n";
        REQUIRE(comparison.good());
    }
    vertices += draw.verts.size();
}

void Case(size_t variant, size_t test, size_t count, bool repeated, bool collapsed = false) {
    ++cases;
    std::fesetround(FE_TOWARDZERO); // NEW generated execution's caller contract
    const auto environment = Input(test, count, repeated);
    if (collapsed) std::memset(ram::bytes.data()+kPositions,0,count*8);
    const uint32_t tint = Next();
    SetupGenerated();
    const auto input = std::make_unique<decltype(ram::bytes)>(ram::bytes);
    const auto model = Run(false, variant, tint, environment,collapsed);
    for (int rounding : {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD}) {
        ram::bytes = *input;
        std::fesetround(rounding);
        const auto output = Run(true, variant, tint, environment,collapsed);
        REQUIRE(std::fegetround() == rounding);
        if (count >= 3 && !collapsed) Compare(model[0], output[0], variant, rounding);
        else {
            REQUIRE(model.empty() && output.empty());
            if (comparison.is_open())
                comparison << cases << '\t' << variant << '\t' << rounding <<
                    "\t0\t0\t0\t0\t0\t0\t0\t0\t0\n";
        }
    }
}

void SetupThenVisible(size_t variant,size_t count,bool repeated) {
    ++cases;
    struct Scope { Scope() { export_run = false; } ~Scope() { export_run = true; } } scope;
    std::fesetround(FE_TOWARDZERO);
    const auto environment = Input(count,4,repeated); // complete following geometry
    ram::write<uint8_t>(kShape+0x10,uint8_t(count));
    const uint32_t tint = Next();
    SetupGenerated();
    const auto input = std::make_unique<decltype(ram::bytes)>(ram::bytes);
    REQUIRE(Run(false,variant,tint,environment).empty());
    ram::write<uint8_t>(kShape+0x10,4);
    const auto model = Run(false,variant,tint,environment);
    for (int rounding : {FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD}) {
        ram::bytes = *input;
        std::fesetround(rounding);
        REQUIRE(Run(true,variant,tint,environment).empty());
        ram::write<uint8_t>(kShape+0x10,4);
        const auto host = Run(true,variant,tint,environment);
        Compare(model[0],host[0],variant,rounding);
        REQUIRE(std::fegetround() == rounding);
    }
}
}  // namespace

int main(int argc, char** argv) {
    REQUIRE(Service::Hooks{}.strip_selection == Service::StripSelection::Plain);
    // [comparison.tsv] [--export-dir DIR --source-manifest FILE]
    const bool tsv = argc >= 2 && std::strncmp(argv[1], "--", 2) != 0;
    REQUIRE(artifacts.Parse(argc, argv, tsv ? 2 : 1));
    if (tsv) {
        comparison.open(argv[1]);
        REQUIRE(comparison.good());
        comparison << "case\tvariant\trounding\tvertices\tmodel_draw_raw\tmodel_draw_active"
            "\tselected_draw\tmodel_vertices\tselected_vertices"
            "\tinactive_texture_flag_difference\tactive_differences\tselected_model_calls\n";
    }
    for (size_t variant = 0; variant < 4; ++variant)
        for (size_t count = 0; count < 3; ++count)
            for (bool repeated : {false,true}) {
                Case(variant, count, count, repeated);
                SetupThenVisible(variant,count,repeated);
            }
    for (size_t variant = 0; variant < 4; ++variant)
        for (bool repeated : {false,true})
            Case(variant,0,4,repeated,true);
    for (size_t variant = 0; variant < 4; ++variant)
        for (size_t test = 0; test < 253; ++test)
            for (bool repeated : {false, true})
                Case(variant, test, test + 3, repeated); // every nonempty count, 3..255
    std::fesetround(FE_TONEAREST);
    std::printf("Strip NEW-body self-oracle: %zu cases, four toggled variants, "
                "first/repeated material, four caller rounding modes, %zu compared vertices; "
                "DrawIR/vertex differences=0; selected model calls=0; "
                "inactive texture flag differences=%zu\n",
                cases, vertices, inactive_fst_differences);
    std::printf("All-mode application route: %zu completed shared-scene draws; "
                "retained publisher draws=0\n",shared_scene_draws);
    artifacts.Finish(argc, argv);
}
