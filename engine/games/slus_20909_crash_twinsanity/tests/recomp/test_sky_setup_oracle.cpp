#include "games/slus_20909_crash_twinsanity/tests/recomp/sky_content_oracle_memory.h"

#include <cfenv>
#include <cmath>
#include <memory>

#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/state/gs_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_setup.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON
namespace sky_content_oracle {
namespace memory { std::array<uint8_t, 0x800000> bytes{}; }
void External(uint32_t, CpuContext&) { std::abort(); }
}
namespace {
namespace ram = sky_content_oracle::memory;
using Setup = title::SkySetup;
size_t cases = 0;
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "SkySetup case%zu line%d: %s\n", cases, __LINE__, #x); std::exit(1); } } while(false)
struct Sink : ps2r::gs::DrawIrSink {
    std::vector<ps2r::gs::DrawIr> draws;
    void OnDraw(const ps2r::gs::DrawIr& draw) override { draws.push_back(draw); }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};
void Compare(unsigned variant) {
    constexpr uint32_t sky = 0x10000, target = 0x11000, view = 0x12000, packet = 0x20000;
    std::memset(ram::bytes.data(), 0, ram::bytes.size());
    const uint32_t width = 128 + variant % 7 * 64, height = 64 + variant % 5 * 73;
    ram::write(title::MaterialState::kDisplayWidth, width);
    ram::write(title::MaterialState::kDisplayHeight, height);
    ram::write(title::MaterialState::kRenderTarget, target);
    ram::write(Setup::kRenderView, view);
    ram::write(target, std::array<uint32_t, 8>{0, variant % 23, variant % 11,
        width - variant % 9, height - variant % 7, 0, width + variant % 3, height + variant % 2});
    title::SkyGeometry::Matrix clip;
    for (unsigned i = 0; i != 4; ++i)
        for (unsigned j = 0; j != 4; ++j) clip[i][j] = std::sin(float(variant * 17 + i * 4 + j)) * 9.73f;
    ram::write(view + 0x80, clip);
    ram::write<uint32_t>(0x323668, 0x22000);
    ram::write(0x323310, packet);
    CpuContext ctx{};
    ctx.vuRegs.VF[0].w = 1;
    ctx.cpuRegs.GPR.r[4].UL[0] = sky;
    ctx.cpuRegs.GPR.r[29].UL[0] = 0x700000;
    std::fesetround(FE_TOWARDZERO);
    sky_content_oracle::Run(Setup::kAddress, ctx);
    const auto expected = ram::read<title::SkyGeometry::Matrix>(sky + 0x10);
    constexpr int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD};
    std::fesetround(modes[variant % 4]);
    Setup::Output selected;
    const auto reader = [](uint32_t address, void* data, size_t size) {
        if (address > ram::bytes.size() || size > ram::bytes.size() - address) return false;
        std::memcpy(data, ram::bytes.data() + address, size);
        return true;
    };
    REQUIRE(Setup::Capture(reader, {}, selected) == Setup::Result::Ok);
    REQUIRE(std::fegetround() == modes[variant % 4]);
    REQUIRE(std::memcmp(expected.data(), selected.to_screen.data(), sizeof(expected)) == 0);
    auto model = std::make_unique<ps2r::gs::GsState>();
    GsStateSink sink(*model); GifRegs regs; GifParser parser(sink, regs);
    const size_t qwc = ram::read<uint32_t>(packet) & 65535;
    REQUIRE(qwc == 6);
    parser.ReceiveData(GifPath::Path2, ram::bytes.data() + packet + 16, qwc * 16);
    Sink output; model->SetSink(&output);
    model->WriteRegister(0, 3);
    for (uint64_t xy : {uint64_t(0), uint64_t(16), uint64_t(16) << 16})
        model->WriteRegister(5, xy);
    model->KickBoundary();
    REQUIRE(output.draws.size() == 1);
    const auto& actual = output.draws[0];
    REQUIRE(actual.off_x == selected.state.off_x && actual.off_y == selected.state.off_y);
    const auto& a = actual.raster; const auto& b = selected.state.raster;
    REQUIRE(a.frame.fbp == b.frame.fbp && a.frame.fbw == b.frame.fbw && a.frame.psm == b.frame.psm);
    REQUIRE(a.zbuf.zbp == b.zbuf.zbp && a.zbuf.psm == b.zbuf.psm && a.zbuf.zmsk == b.zbuf.zmsk);
    REQUIRE(a.scissor.x0 == b.scissor.x0 && a.scissor.x1 == b.scissor.x1 &&
            a.scissor.y0 == b.scissor.y0 && a.scissor.y1 == b.scissor.y1 && !a.dthe);
    ++cases;
}
}
int main() {
    const int saved = std::fegetround();
    for (unsigned i = 0; i != 280; ++i) Compare(i);
    std::fesetround(saved);
    std::printf("SkySetup NEW producer self-oracle: %zu cases; matrix + target state equal\n", cases);
}
