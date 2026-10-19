#include "games/slus_20909_crash_twinsanity/tests/recomp/frame_oracle_memory.h"

#include <fstream>
#include <memory>
#include <map>
#include <cfenv>
#include <cstring>

#include "engine/core/hash/hash.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/state/gs_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_intent.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_service.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.h"

namespace frame_oracle::memory { std::array<uint8_t,0x400000> bytes{}; }
namespace {
using Intent = title::FrameIntent;
using Draw = ps2r::gs::DrawIr;
using Environment = title::MaterialState::Environment;
namespace ram = frame_oracle::memory;
size_t checks = 0, failures = 0, cases = 0, vertices = 0;
retirement::OracleArtifacts artifacts;
bool record_case = false;  // the FE_TOWARDZERO case of each (width, height): both arms recorded once
title::FrameService::Counters census[2];  // accumulated actual FrameService census per arm (recorded cases)
void Accumulate(bool native, const title::FrameService::Counters& c) {
    auto add = [](title::FrameService::Count& to, const title::FrameService::Count& from) {
        to.calls += from.calls; to.replaced += from.replaced; to.model_calls += from.model_calls; };
    add(census[native].setup, c.setup); add(census[native].send, c.send); add(census[native].interrupt, c.interrupt);
    add(census[native].clear, c.clear); add(census[native].layout, c.layout);
    census[native].foreign_sends += c.foreign_sends; census[native].interior_calls += c.interior_calls;
    census[native].failures += c.failures;
}
std::string Row(const title::FrameService::Count& c) {
    return "calls=" + std::to_string(c.calls) + ",replaced=" + std::to_string(c.replaced) + ",model_calls=" + std::to_string(c.model_calls);
}
template <class T> void Word(std::vector<uint8_t>& out, T value) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}
void Record(bool native, const Draw& draw) { artifacts.RecordDraw(native, draw, true, false); }
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL case%zu line%d: %s\n",cases,__LINE__,#x); } } while(false)
class TestFrameSelfOracle final : public ps2r::gs::DrawIrSink {
public:
    std::vector<Draw> draws;
    void OnDraw(const Draw& draw) override { draws.push_back(draw); }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};

auto Reader() {
    return [](uint32_t address,void* out,size_t size) {
        if (address > ram::bytes.size() || size > ram::bytes.size()-address) return false;
        std::memcpy(out,ram::bytes.data()+address,size);
        return true;
    };
}
Draw Run(bool selected, uint32_t width,uint32_t height,const Environment& environment) {
    // Both paths receive the same source input and pristine independent RAM.
    std::memset(ram::bytes.data(),0,ram::bytes.size());
    ram::write<uint32_t>(title::MaterialState::kDisplayWidth,width);
    ram::write<uint32_t>(title::MaterialState::kDisplayHeight,height);
    ram::write<uint32_t>(title::MaterialState::kDisplayPixelBytes,2);
    ram::write<uint32_t>(title::MaterialState::kDisplayPixelBytes+4,4);
    CpuContext context{};
    context.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    if (artifacts.Enabled() && record_case) {
        // The consumed input: the four display globals the body reads, the
        // caller's gp and the frame environment; the remaining RAM is zero.
        auto& in = artifacts.Input(selected);
        Word(in, uint32_t(0x454d5246));
        Word(in, width); Word(in, height); Word(in, uint32_t(2)); Word(in, uint32_t(4));
        Word(in, context.cpuRegs.GPR.r[28].UL[0]);
        Word(in, uint8_t(environment.colour_clamp)); Word(in, uint8_t(environment.per_pixel_alpha));
        Word(in, environment.scan_mask); Word(in, environment.dither); Word(in, environment.dither_matrix);
        Word(in, environment.fog_r); Word(in, environment.fog_g); Word(in, environment.fog_b);
    }
    using Service = title::FrameService;
    std::map<uint32_t,Service::Body> entries;
    unsigned generated = 0, unexpected = 0;
    Draw owned;
    Service service({selected,Reader(),[&](auto& frame) {
        const uint32_t pixels = width*height;
        frame = {pixels*2/8192*8192,pixels*6/8192*8192,width,height,0,1};
        return true;
    },[&](auto plan,const auto&) { owned = plan->draw; return true; }});
    CHECK(service.Install([&](uint32_t at) -> Service::Body {
        if (at == Intent::kAddress) return [&](CpuContext& c,uint32_t pc) {
            ++generated; frame_oracle::FUN_0019aaa8(c,pc);
        };
        return [&](CpuContext&,uint32_t) { ++unexpected; };
    },[&](uint32_t at,auto body) { entries[at] = std::move(body); return true; }));
    if (selected) {
        const auto input = ps2r::hash::Fnv1a64(ram::bytes.data(),ram::bytes.size());
        entries.at(Intent::kAddress)(context,Intent::kAddress);
        context.cpuRegs.GPR.r[4].UL[0] = Intent::kSourceObject;
        entries.at(Service::kSend)(context,Service::kSend);
        CHECK(input == ps2r::hash::Fnv1a64(ram::bytes.data(),ram::bytes.size()));
        CHECK(service.Counts().setup.replaced == 1 && service.Counts().send.replaced == 1 &&
              service.Counts().setup.model_calls == 0 && service.Counts().send.model_calls == 0 &&
              generated == 0 && unexpected == 0);
        if (record_case) {
            Accumulate(true, service.Counts());
            if (artifacts.Enabled()) Record(true, owned);
        }
        return owned;
    }
    entries.at(Intent::kAddress)(context,Intent::kAddress);
    CHECK(generated == 1 && service.Counts().setup.model_calls == 1 && unexpected == 0);
    if (record_case) Accumulate(false, service.Counts());
    TestFrameSelfOracle result;
    auto model = std::make_unique<ps2r::gs::GsState>();
    model->SetSink(&result);
    model->WriteRegister(0x22,environment.scan_mask);
    model->WriteRegister(0x49,environment.per_pixel_alpha);
    model->WriteRegister(0x3d,uint64_t(environment.fog_r) |
        uint64_t(environment.fog_g)<<8 | uint64_t(environment.fog_b)<<16);
    // Inactive context-two blend and mip addresses are deliberately poisoned.
    model->WriteRegister(0x43,0x7f00000044ull);
    model->WriteRegister(0x35,0x123456789abcdefull);
    model->WriteRegister(0x37,0xfedcba987654321ull);
    GsStateSink sink(*model);
    GifRegs regs;
    GifParser parser(sink,regs);
    const auto words = ram::read<uint32_t>(Intent::kSourceObject) & 0xffff;
    CHECK(words == width/32*4+20);
    unsigned dithers = 0;
    for (uint32_t i = 1; i < words; ++i) {
        const auto at = Intent::kSourceObject + 16 + i * 16;
        if (ram::read<uint64_t>(at+8) != 0x45) continue;
        CHECK(ram::read<uint64_t>(at) == (dithers == 0 ? 1u : 0u));
        ++dithers;
    }
    CHECK(dithers == 2); // authored on while drawing, off only after the final vertex
    parser.ReceiveData(GifPath::Path3,ram::bytes.data()+Intent::kSourceObject+16,words*16);
    CHECK(parser.CanAcceptDirect() && result.draws.size() == 1);
    if (artifacts.Enabled() && record_case && !result.draws.empty()) Record(false, result.draws[0]);
    return result.draws.empty() ? Draw{} : result.draws[0];
}

void Compare(uint32_t width,uint32_t height,int rounding,const Environment& environment,
             std::ofstream& evidence) {
    // The generated guest thread executes under truncation. Owned producers
    // must preserve their caller's mode and work under every supported mode.
    CHECK(std::fesetround(FE_TOWARDZERO) == 0);
    record_case = rounding == FE_TOWARDZERO;
    auto model = Run(false,width,height,environment);
    CHECK(std::fesetround(rounding) == 0);
    const auto host = Run(true,width,height,environment);
    CHECK(std::fegetround() == rounding);
    const auto raw = model.ToDrawRec();
    // Source disables blend/mips. Only those inactive values are normalized.
    model.raster.alpha = {};
    for (size_t i = 0; i < 6; ++i) model.tex.mip_tbp[i] = model.tex.mip_tbw[i] = 0;
    const auto a = model.ToDrawRec(),b = host.ToDrawRec();
    const bool equal = std::memcmp(&a,&b,sizeof(a)) == 0;
    if (!equal && failures < 8)
        for (size_t i = 0; i < sizeof(a); ++i)
            if (reinterpret_cast<const uint8_t*>(&a)[i] != reinterpret_cast<const uint8_t*>(&b)[i])
                std::fprintf(stderr,"draw byte%zu: %02x/%02x\n",i,
                    reinterpret_cast<const uint8_t*>(&a)[i],reinterpret_cast<const uint8_t*>(&b)[i]);
    CHECK(equal);
    // Attributed active difference: NEW GsRegisters::WriteEnvironment does
    // not flush on DTHE. The final disable precedes DrawFlush::FillRaster.
    // Do not deepen that model or copy its defect into the source intent.
    CHECK(model.raster.dthe == 0 && host.raster.dthe == 1 &&
          model.raster.dimx == host.raster.dimx && host.raster.dimx == 0x6071243571603524ull);
    CHECK(model.verts.size() == host.verts.size() && model.verts.size() == width/32*6);
    CHECK(model.verts.size() == host.verts.size() &&
          std::memcmp(model.verts.data(),host.verts.data(),host.verts.size()*sizeof(host.verts[0])) == 0);
    CHECK(model.tex.texa_ta0 == host.tex.texa_ta0 && model.tex.texa_ta1 == host.tex.texa_ta1);
    CHECK(model.tex.mxl == host.tex.mxl && model.tex.lcm == host.tex.lcm &&
          model.tex.l == host.tex.l && model.tex.k == host.tex.k);
    vertices += host.verts.size();
    if (evidence) evidence << width << '\t' << height << '\t' << rounding
        << '\t' << unsigned(environment.scan_mask)
        << '\t' << std::hex << ps2r::hash::Fnv1a64(&raw,sizeof(raw))
        << '\t' << ps2r::hash::Fnv1a64(&a,sizeof(a)) << '\t' << ps2r::hash::Fnv1a64(&b,sizeof(b))
        << '\t' << model.snapshot.vertex_hash << '\t' << host.snapshot.vertex_hash
        << std::dec << '\t' << equal << '\t' << unsigned(model.raster.dthe)
        << '\t' << unsigned(host.raster.dthe) << "\ttrailing-dither-disable\n";
    ++cases;
}
}

int main(int argc,char** argv) {
    std::ofstream evidence;
    // [evidence.tsv] [--export-dir DIR --source-manifest FILE]
    const bool tsv = argc >= 2 && std::strncmp(argv[1], "--", 2) != 0;
    CHECK(artifacts.Parse(argc, argv, tsv ? 2 : 1));
    if (tsv) {
        evidence.open(argv[1]);
        CHECK(bool(evidence));
        evidence << "width\theight\trounding\tscan_mask\tmodel_raw\tmodel_active\thost_active"
                    "\tmodel_vertices\thost_vertices\tdraw_vertex_equal\tmodel_dither"
                    "\thost_dither\tattribution\n";
    }
    const int original_rounding = std::fegetround();
    for (uint32_t width : {64u,128u,256u,512u,640u,1024u})
        for (uint32_t height : {1u,32u,224u,400u,448u,512u})
            for (int rounding : {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
                Environment environment;
                if (uint64_t(width)*height*10 <= 4*1024*1024)
                    Compare(width,height,rounding,environment,evidence);
            }
    CHECK(std::fesetround(original_rounding) == 0);
    Intent::Output output;
    output.width = 91;
    for (auto dimensions : {std::pair{0u,448u},std::pair{63u,32u},std::pair{512u,0u},
                             std::pair{1024u,1024u},std::pair{UINT32_MAX,UINT32_MAX}})
        CHECK(Intent::Build(dimensions.first,dimensions.second,{},output) != Intent::Result::Ok &&
              output.width == 91);
    CHECK(Intent::Capture({},Intent::kAddress,{},output) == Intent::Result::InvalidMemory);
    CHECK(Intent::Capture(Reader(),0x19acf4,{},output) == Intent::Result::UnsupportedEntry);
    std::printf("FrameSelfOracle: %zu cases, %zu vertices, %zu checks, %zu failures\n",
                cases,vertices,checks,failures);
    if (!failures && artifacts.Enabled()) {
        // The model arm runs once per case (always FE_TOWARDZERO); the native arm is
        // recorded only in its FE_TOWARDZERO case, so both channels hold one record per
        // (width, height) and the same consumed input.
        for (bool native : {false, true}) {
            const auto& c = census[native];
            artifacts.Counter(native, "graphics_frame_setup", Row(c.setup));
            artifacts.Counter(native, "graphics_frame_send", Row(c.send));
            artifacts.Counter(native, "graphics_frame_interrupt", Row(c.interrupt));
            artifacts.Counter(native, "graphics_frame_clear", Row(c.clear));
            artifacts.Counter(native, "graphics_frame_layout", Row(c.layout));
            artifacts.Counter(native, "graphics_frame_other", "foreign_sends=" + std::to_string(c.foreign_sends) +
                ",interior_calls=" + std::to_string(c.interior_calls) + ",failed=" + std::to_string(c.failures));
            // The fixture's owned-plan sink stands for the completed display copy.
            artifacts.Counter(native, "graphics_frame_output", "completed=" + std::to_string(c.setup.replaced) +
                ",setups=0,layouts=0");
        }
        artifacts.Setting("hle.graphics_frame", "off", "on");
        artifacts.Setting("rounding", "FE_TOWARDZERO", "FE_TOWARDZERO");
        artifacts.Note("scope", "source WriteFrameHead 0x0019aaa8 CT24->CT16 display copy: canonical DrawRec plus "
            "out-of-record dither/mip/TEXA fields and vertices; no pixel/presentation claim");
        artifacts.Note("canonicalization", "inactive ALPHA (PRIM.ABE=0) and unaddressed (MXL=0 or MMIN<2) mip addresses zeroed on both arms; "
            "everything else raw, so the attributed trailing-dither byte is the only expected difference");
        artifacts.Note("counter_provenance", "graphics_frame_*: FrameService::Run census, published by "
            "hle/graphics/graphics_report.cpp; the model arm invokes only WriteFrameHead (send/interrupt/clear/layout "
            "stay zero there and are NOT exercised by this oracle); graphics_frame_output.completed counts owned "
            "plans the fixture's sink received");
        artifacts.Attribute({"draw",
            "the generated writer emits DTHE=1 before the strip vertices and DTHE=0 after them; NEW's "
            "GsRegisters::WriteEnvironment does not flush on DTHE, so the model's DrawFlush observes the trailing "
            "disable (dthe=0) while the source intent draws with dithering enabled (dthe=1), TR frame.cpp Dither/GsDither",
            "twinsanity-reversed/src/platform/ps2/renderer/frame.cpp",
            "packet.Write(Dither, GsDither);", "FrameSelfOracle"});
        artifacts.Finish("FrameSelfOracle", "frame-artifacts-v1", argc, argv);
    }
    return failures ? 1 : 0;
}
