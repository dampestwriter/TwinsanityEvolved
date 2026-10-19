#include "games/slus_20909_crash_twinsanity/tests/recomp/frame_oracle_memory.h"

#include <cfenv>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <cstring>

#include "engine/core/hash/hash.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/state/gs_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_service.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.h"

namespace frame_oracle::memory { std::array<uint8_t,0x400000> bytes{}; }
namespace {
using Intent = title::FrameClear;
using Source = title::FrameEnvironment;
using Service = title::FrameService;
using Draw = ps2r::gs::DrawIr;
namespace ram = frame_oracle::memory;
size_t checks = 0, failures = 0, cases = 0, vertices = 0, inactive_fst = 0;
retirement::OracleArtifacts artifacts;
bool record_case = false;  // the FE_TOWARDZERO case of each input: both arms recorded once
Service::Counters census[2];
void Accumulate(bool native, const Service::Counters& c) {
    auto add = [](Service::Count& to, const Service::Count& from) {
        to.calls += from.calls; to.replaced += from.replaced; to.model_calls += from.model_calls; };
    add(census[native].setup, c.setup); add(census[native].send, c.send); add(census[native].interrupt, c.interrupt);
    add(census[native].clear, c.clear); add(census[native].layout, c.layout);
    census[native].foreign_sends += c.foreign_sends; census[native].interior_calls += c.interior_calls;
    census[native].failures += c.failures;
}
std::string Row(const Service::Count& c) {
    return "calls=" + std::to_string(c.calls) + ",replaced=" + std::to_string(c.replaced) + ",model_calls=" + std::to_string(c.model_calls);
}
template <class T> void Word(std::vector<uint8_t>& out, T value) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL case%zu line%d: %s\n",cases,__LINE__,#x); } } while(false)
class TestFrameClearSelfOracle final : public ps2r::gs::DrawIrSink {
public:
    std::vector<Draw> draws;
    void OnDraw(const Draw& draw) override { draws.push_back(draw); }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};
constexpr uint32_t target = 0x12000, packet = 0x18000, tail = 0x10000;
auto Reader() {
    return [](uint32_t at,void* out,size_t size) {
        if (at > ram::bytes.size() || size > ram::bytes.size()-at) return false;
        std::memcpy(out,ram::bytes.data()+at,size);
        return true;
    };
}
void Seed(const Intent::Input& in) {
    std::memset(ram::bytes.data(),0,ram::bytes.size());
    ram::write(target+0x60,in.x); ram::write(target+0x64,in.y);
    ram::write(target+0xc,in.width); ram::write(target+0x10,in.height);
    ram::write(target+0x18,in.screen_width); ram::write(target+0x1c,in.screen_height);
    ram::write(target+0x14,in.red); ram::write(target+0x15,in.green); ram::write(target+0x16,in.blue);
    ram::write(title::MaterialState::kDisplayWidth,in.screen_width);
    ram::write(title::MaterialState::kDisplayHeight,in.screen_height);
    ram::write<uint32_t>(title::MaterialState::kDisplayPixelBytes,2);
    ram::write<uint32_t>(Source::kDrawPixelBytes,4);
    const auto pixels = in.screen_width*in.screen_height;
    ram::write<uint32_t>(Source::kFrameBufferPage,pixels*2/8192);
    ram::write<uint32_t>(title::MaterialState::kDepthBufferPage,pixels*6/8192);
    ram::write<uint32_t>(Source::kFrameBuckets+16,tail); // bucket zero's last chain link
    ram::write<uint32_t>(0x323300+0x10,packet);
}

std::vector<Draw> Run(bool selected,const Intent::Input& in) {
    Seed(in); // identical independent source input, including empty producers
    Source source(Reader());
    std::map<uint32_t,Service::Body> dispatch;
    unsigned generated = 0, other = 0, captures = 0;
    std::vector<Draw> result;
    Service service({selected,Reader(),[](auto&) { return false; },
        [](auto,const auto&) { return false; },
        [&](auto& frame) { return source.CaptureSetup(frame); },
        [&](auto intent,const auto& frame) {
            CHECK(frame.source_frame == 0 && frame.epoch == 1);
            CHECK(frame.width == in.screen_width && frame.height == in.screen_height);
            CHECK(source.CurrentStatus() == Source::Status::IncompleteOperation);
            if (!intent->draw.verts.empty()) result.push_back(intent->draw);
            ++captures;
            return true;
        }});
    auto capture = [&](uint32_t at) -> Service::Body {
        if (dispatch.count(at)) return dispatch.at(at);
        if (at == Intent::kAddress) return [&](auto& c,auto pc) {
            ++generated; frame_oracle::FUN_0019f1c8(c,pc);
        };
        return [&](auto&,auto) { ++other; };
    };
    auto install = [&](uint32_t at,Service::Body body) { dispatch[at] = std::move(body); return true; };
    CHECK(service.Install(capture,install));
    CHECK(source.Install(capture,install)); // actual root order: observer outside replacement
    Source::SceneFrame frame;
    CHECK(!source.CaptureSetup(frame));
    CpuContext context{};
    context.cpuRegs.GPR.r[4].UL[0] = target;
    context.cpuRegs.GPR.r[5].UD[0] = in.color ? 0x100000000ull : 0; // full source truth test
    context.cpuRegs.GPR.r[6].UD[0] = in.depth ? UINT64_MAX : 0;
    context.cpuRegs.GPR.r[29].UL[0] = 0x3f0000;
    if (artifacts.Enabled() && record_case) {
        // The consumed input: Seed()'s source target/globals and the call registers.
        auto& out = artifacts.Input(selected);
        Word(out, uint32_t(0x524c4346));
        Word(out, in.x); Word(out, in.y); Word(out, in.width); Word(out, in.height);
        Word(out, in.screen_width); Word(out, in.screen_height);
        Word(out, in.red); Word(out, in.green); Word(out, in.blue); Word(out, uint8_t(in.color)); Word(out, uint8_t(in.depth));
        Word(out, context.cpuRegs.GPR.r[4].UL[0]); Word(out, context.cpuRegs.GPR.r[5].UD[0]);
        Word(out, context.cpuRegs.GPR.r[6].UD[0]); Word(out, context.cpuRegs.GPR.r[29].UL[0]);
    }
    const auto before = ps2r::hash::Fnv1a64(ram::bytes.data(),ram::bytes.size());
    dispatch.at(Intent::kAddress)(context,Intent::kAddress);
    CHECK(!source.CaptureSetup(frame) && source.CaptureScene(24,frame));
    CHECK(frame.epoch == 1 && service.Counts().clear.calls == 1 && other == 0);
    if (selected) {
        CHECK(captures == 1 && generated == 0 && service.Counts().clear.replaced == 1 &&
              service.Counts().clear.model_calls == 0);
        CHECK(before == ps2r::hash::Fnv1a64(ram::bytes.data(),ram::bytes.size()));
        if (record_case) {
            Accumulate(true, service.Counts());
            if (artifacts.Enabled()) {
                Word(artifacts.Output(true, "draw"), uint32_t(result.size()));
                for (const auto& draw : result) artifacts.RecordDraw(true, draw, true, true);
            }
        }
        return result;
    }
    CHECK(generated == 1 && captures == 0 && service.Counts().clear.model_calls == 1);
    if (record_case) Accumulate(false, service.Counts());
    const auto words = ram::read<uint32_t>(packet) & 0xffff;
    CHECK(words == in.width/32*2+14);
    bool texa = false;
    for (uint32_t i = 1; i < words; ++i) {
        const auto at = packet + 16 + i*16;
        if (ram::read<uint64_t>(at+8) == 0x3b) {
            texa = true; CHECK(ram::read<uint64_t>(at) == 0x8000000000ull);
        }
    }
    CHECK(texa);
    TestFrameClearSelfOracle output;
    auto model = std::make_unique<ps2r::gs::GsState>();
    model->SetSink(&output);
    model->WriteRegister(0x44,0x6071243571603524ull);
    // Deliberately poison inactive inherited blending and texture addresses.
    model->WriteRegister(0x43,0x7f00000044ull);
    model->WriteRegister(0x07,0x123456789abcdefull);
    GsStateSink sink(*model);
    GifRegs regs;
    GifParser parser(sink,regs);
    parser.ReceiveData(GifPath::Path3,ram::bytes.data()+packet+16,words*16);
    CHECK(parser.CanAcceptDirect());
    if (artifacts.Enabled() && record_case) {
        Word(artifacts.Output(false, "draw"), uint32_t(output.draws.size()));
        for (const auto& draw : output.draws) artifacts.RecordDraw(false, draw, true, true);
    }
    return output.draws;
}

void Compare(const Intent::Input& in,int rounding,std::ofstream& evidence) {
    CHECK(std::fesetround(FE_TOWARDZERO) == 0);
    record_case = rounding == FE_TOWARDZERO;
    auto model = Run(false,in);
    CHECK(std::fesetround(rounding) == 0);
    const auto host = Run(true,in);
    CHECK(std::fegetround() == rounding);
    CHECK(model.size() == host.size() && host.size() == (in.width >= 32 ? 1u : 0u));
    bool equal = model.size() == host.size();
    uint64_t model_hash = 0,host_hash = 0,raw_hash = 0,active_hash = 0,host_draw_hash = 0;
    if (model.size() == 1 && host.size() == 1) {
        // Untextured/unblended source: texture descriptors and blend equation
        // are inactive. PRIM's coordinate mode and every vertex stay compared.
        // NEW tex.fst records the last UV/ST write even without texturing.
        // This producer writes neither, so that inactive history differs.
        CHECK(model[0].use_fst == host[0].use_fst && !model[0].tex_enabled);
        inactive_fst += model[0].tex.fst != host[0].tex.fst;
        const auto raw = model[0].ToDrawRec();
        raw_hash = ps2r::hash::Fnv1a64(&raw,sizeof(raw));
        model[0].raster.alpha = {};
        model[0].tex = host[0].tex;
        const auto a = model[0].ToDrawRec(),b = host[0].ToDrawRec();
        active_hash = ps2r::hash::Fnv1a64(&a,sizeof(a));
        host_draw_hash = ps2r::hash::Fnv1a64(&b,sizeof(b));
        equal = std::memcmp(&a,&b,sizeof(a)) == 0;
        if (!equal && failures < 8)
            for (size_t i = 0; i < sizeof(a); ++i)
                if (reinterpret_cast<const uint8_t*>(&a)[i] != reinterpret_cast<const uint8_t*>(&b)[i])
                    std::fprintf(stderr,"draw byte%zu: %02x/%02x\n",i,
                        reinterpret_cast<const uint8_t*>(&a)[i],reinterpret_cast<const uint8_t*>(&b)[i]);
        CHECK(equal);
        CHECK(model[0].raster.dthe == host[0].raster.dthe &&
              model[0].raster.dimx == host[0].raster.dimx);
        CHECK(model[0].verts.size() == host[0].verts.size());
        const bool geometry = model[0].verts.size() == host[0].verts.size() &&
            std::memcmp(model[0].verts.data(),host[0].verts.data(),
                        host[0].verts.size()*sizeof(host[0].verts[0])) == 0;
        CHECK(geometry);
        equal &= geometry;
        vertices += host[0].verts.size();
        model_hash = model[0].snapshot.vertex_hash; host_hash = host[0].snapshot.vertex_hash;
    }
    if (evidence) evidence << in.screen_width << '\t' << in.screen_height << '\t'
        << in.x << '\t' << in.y << '\t' << in.width << '\t' << in.height << '\t'
        << in.color << '\t' << in.depth << '\t' << rounding << '\t' << std::hex
        << raw_hash << '\t' << active_hash << '\t' << host_draw_hash << '\t'
        << model_hash << '\t' << host_hash << std::dec << '\t' << equal << '\n';
    ++cases;
}
}
int main(int argc,char** argv) {
    std::ofstream evidence;
    // [evidence.tsv] [--export-dir DIR --source-manifest FILE]
    const bool tsv = argc >= 2 && std::strncmp(argv[1], "--", 2) != 0;
    CHECK(artifacts.Parse(argc, argv, tsv ? 2 : 1));
    if (tsv) {
        evidence.open(argv[1]); CHECK(bool(evidence));
        evidence << "screen_w\tscreen_h\tx\ty\twidth\theight\tcolor\tdepth\trounding"
                    "\tmodel_raw\tmodel_active\thost_active\tmodel_vertices\thost_vertices\tequal\n";
    }
    const int original = std::fegetround();
    for (const auto& size : {std::pair{64u,32u},std::pair{512u,448u},std::pair{640u,400u}})
        for (uint32_t width : {0u,1u,31u,32u,63u,64u,size.first})
            for (unsigned mask = 0; mask < 4; ++mask)
                for (int rounding : {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
                    Intent::Input in{2048-size.first/2,2048-size.second/2,
                        width,size.second,size.first,size.second,
                        uint8_t(width+17),uint8_t(mask*73),uint8_t(255-width),bool(mask&1),bool(mask&2)};
                    if (width == 63) { in.x -= 7; in.y += 11; in.height = 13; }
                    Compare(in,rounding,evidence);
                }
    CHECK(std::fesetround(original) == 0);
    Intent::Output untouched; untouched.width = 99;
    Intent::Input invalid{2048,2048,UINT32_MAX,448,512,448,0,0,0,true,true};
    CHECK(Intent::Build(invalid,untouched) == Intent::Result::UnsupportedFrame && untouched.width == 99);
    CpuContext context{};
    CHECK(Intent::Capture(Reader(),context,Intent::kAddress+4,untouched) ==
          Intent::Result::UnsupportedEntry && untouched.width == 99);
    CHECK(Intent::Capture(Reader(),context,Intent::kAddress,untouched) == Intent::Result::InvalidMemory);
    std::printf("FrameClearSelfOracle: %zu cases, %zu vertices, %zu inactive FST differences, "
                "%zu checks, %zu failures\n",cases,vertices,inactive_fst,checks,failures);
    if (!failures && artifacts.Enabled()) {
        for (bool native : {false, true}) {
            const auto& c = census[native];
            artifacts.Counter(native, "graphics_frame_setup", Row(c.setup));
            artifacts.Counter(native, "graphics_frame_send", Row(c.send));
            artifacts.Counter(native, "graphics_frame_interrupt", Row(c.interrupt));
            artifacts.Counter(native, "graphics_frame_clear", Row(c.clear));
            artifacts.Counter(native, "graphics_frame_layout", Row(c.layout));
            artifacts.Counter(native, "graphics_frame_other", "foreign_sends=" + std::to_string(c.foreign_sends) +
                ",interior_calls=" + std::to_string(c.interior_calls) + ",failed=" + std::to_string(c.failures));
            // The fixture's owned-intent sink stands for the completed shared-target clear.
            artifacts.Counter(native, "graphics_frame_output", "completed=0,setups=" + std::to_string(c.clear.replaced) + ",layouts=0");
        }
        artifacts.Setting("hle.graphics_frame", "off", "on");
        artifacts.Setting("rounding", "FE_TOWARDZERO", "FE_TOWARDZERO");
        artifacts.Note("scope", "source SetUpFrame 0x0019f1c8 bucket-zero clear rectangle/masks on the shared scene: "
            "canonical DrawRec plus out-of-record fields and vertices (zero draws for widths < 32); no pixel claim");
        artifacts.Note("canonicalization", "inactive ALPHA (PRIM.ABE=0), inactive texture view (PRIM.TME=0) and unaddressed (MXL=0 or MMIN<2) mip "
            "addresses zeroed on both arms (the model fixture poisons the unused ALPHA/TEX0 registers; NEW tex.fst records "
            "the last UV/ST write the source never makes); everything else raw");
        artifacts.Note("counter_provenance", "graphics_frame_*: FrameService::Run census, published by "
            "hle/graphics/graphics_report.cpp; this oracle exercises only the clear boundary; graphics_frame_output.setups "
            "counts owned clear intents the fixture's sink received");
        artifacts.Finish("FrameClearSelfOracle", "frame-clear-artifacts-v1", argc, argv);
    }
    return failures ? 1 : 0;
}
