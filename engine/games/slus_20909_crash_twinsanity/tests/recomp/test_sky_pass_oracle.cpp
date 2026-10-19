// Complete NEW source producers + unchanged NEW transport/shaders versus the
// selected source adapters, on identical ELF/data/objects. No game execution.
#include "games/slus_20909_crash_twinsanity/tests/recomp/sky_content_oracle_memory.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

#include "engine/dmac/chain_walker.h"
#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/gif/gif_arbiter.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/state/gs_state.h"
#include "engine/gs/hle/texsrc/texture_source.h"
#include "engine/vif/vif.h"
#include "engine/vu/tiers/vu_tier_dispatch.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_renderer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_pass.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_state.h"
#include "tests/lib/fake_backend.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON
namespace sky_content_oracle {
namespace memory { std::array<uint8_t, 0x800000> bytes{}; }
uint32_t allocation = 0x500000;
void External(uint32_t address, CpuContext& context) {
    if (address != 0x1815c0 || context.cpuRegs.GPR.r[4].UL[0] > 4096) {
        std::fprintf(stderr, "Unexpected generated external %08x\n", address); std::abort();
    }
    context.cpuRegs.GPR.r[2].UL[0] = allocation;
    allocation += (context.cpuRegs.GPR.r[4].UL[0] + 15) & ~15u;
}
}
namespace {
namespace ram = sky_content_oracle::memory;
using Pass = title::SkyPass;
using Assets = title::SkyAssets;
using Draw = ps2r::gs::DrawIr;
using Matrix = title::SkyGeometry::Matrix;
using Graphics = title::Graphics;
using Targets = title::GraphicsTargets;
using Resources = title::GraphicsResources;
constexpr uint32_t kSky = 0x10000, kRigid = 0x11000, kModel = 0x12000;
constexpr uint32_t kMaterial = 0x13000, kShader = 0x14000, kTexture = 0x15000;
constexpr uint32_t kTarget = 0x16000, kView = 0x17000, kReference = 0x18000;
constexpr uint32_t kScreen = 0x19000, kClip = 0x1a000, kList = 0x1b000;
constexpr uint32_t kRoot = 0x20000, kPacket = 0x21000, kBucket = 0x323658;
size_t cases = 0, vertices_compared = 0, model_bodies = 0, residency_uploads = 0;
std::ofstream comparison;
retirement::OracleArtifacts artifacts;
std::unique_ptr<decltype(ram::bytes)> elf_image;  // the loaded ELF alone: the per-case input is its delta
title::SkyPass::Counters census[2];
template <class T> void Word(std::vector<uint8_t>& out, T value) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}
void Accumulate(bool native, const title::SkyPass::Counters& c) {
    auto add = [](title::SkyPass::Counts& to, const title::SkyPass::Counts& from) {
        to.calls += from.calls; to.replaced += from.replaced; to.model_calls += from.model_calls;
        to.interior_calls += from.interior_calls; };
    add(census[native].start, c.start); add(census[native].model, c.model); add(census[native].finish, c.finish);
    census[native].models_loaded += c.models_loaded; census[native].textures_loaded += c.textures_loaded;
    census[native].load_declines += c.load_declines; census[native].faults += c.faults;
}
std::string Row(const title::SkyPass::Counts& c) {
    return "replaced=" + std::to_string(c.replaced) + ",model_calls=" + std::to_string(c.model_calls) +
           ",interior_calls=" + std::to_string(c.interior_calls);
}
// Consumed input: the ELF image once (digest in every record), then the 4 KiB pages
// this case's fixture changed relative to it. Identical on both arms by construction.
void RecordInput(bool native) {
    auto& out = artifacts.Input(native);
    Word(out, uint32_t(0x59534b59));
    constexpr size_t kPage = 4096;
    std::vector<uint32_t> pages;
    for (size_t page = 0; page < ram::bytes.size() / kPage; ++page)
        if (std::memcmp(ram::bytes.data() + page * kPage, elf_image->data() + page * kPage, kPage) != 0)
            pages.push_back(uint32_t(page));
    Word(out, uint32_t(pages.size()));
    for (uint32_t page : pages) {
        Word(out, page);
        out.insert(out.end(), ram::bytes.data() + page * kPage, ram::bytes.data() + (page + 1) * kPage);
    }
}
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"SkyPass case%zu line%d: %s\n",cases,__LINE__,#x); std::exit(1); } } while(false)
std::vector<uint8_t> File(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
template<class T> T Field(const std::vector<uint8_t>& bytes, size_t offset) {
    REQUIRE(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset);
    T out; std::memcpy(&out, bytes.data() + offset, sizeof(T)); return out;
}
auto Reader() {
    return [](uint32_t address, void* data, size_t size) {
        if (address > ram::bytes.size() || size > ram::bytes.size() - address) return false;
        std::memcpy(data, ram::bytes.data() + address, size); return true;
    };
}
CpuContext Context() {
    CpuContext result{};
    result.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    result.cpuRegs.GPR.r[29].UL[0] = 0x700000;
    result.vuRegs.VF[0].w = 1;
    return result;
}
title::MaterialState::Environment Environment() {
    title::MaterialState::Environment env;
    env.colour_clamp = cases % 2;
    env.per_pixel_alpha = cases % 3 == 0;
    env.scan_mask = cases % 4;
    env.fog_r = uint8_t(cases * 13); env.fog_g = 128; env.fog_b = 255;
    env.dither_matrix = 0x3625140736251407ull;
    return env;
}
void Call(uint32_t entry, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
    auto context = Context();
    context.cpuRegs.GPR.r[4].UL[0] = a0; context.cpuRegs.GPR.r[5].UL[0] = a1;
    context.cpuRegs.GPR.r[6].UL[0] = a2; context.cpuRegs.GPR.r[7].UL[0] = a3;
    sky_content_oracle::Run(entry, context);
}
void Elf(const std::vector<uint8_t>& file) {
    const size_t table = Field<uint32_t>(file, 28);
    for (size_t i = 0; i < Field<uint16_t>(file, 44); ++i) {
        const size_t at = table + i * Field<uint16_t>(file, 42);
        if (Field<uint32_t>(file, at) != 1) continue;
        const auto offset = Field<uint32_t>(file, at + 4), address = Field<uint32_t>(file, at + 8);
        const auto size = Field<uint32_t>(file, at + 16);
        REQUIRE(offset <= file.size() && size <= file.size() - offset);
        std::memcpy(ram::bytes.data() + ram::Address(address, size), file.data() + offset, size);
    }
}
void Disk(int32_t handle, uint32_t address, uint32_t size) {
    const uint32_t node = 0x40000 + uint32_t(handle) * 32;
    ram::write(Assets::kDiskManager + 0x58 + uint32_t(handle) * 4, node);
    ram::write(node, std::array<uint32_t,8>{uint32_t(handle), address, size,0,0,0,0,0x4001});
}
struct Material {
    std::vector<uint8_t> bytes;
    uint32_t texture = 0;
    size_t settings = 0;
};
Material ReadMaterial(const std::filesystem::path& path) {
    Material material{File(path)};
    const size_t start = 16 + Field<uint32_t>(material.bytes, 12);
    REQUIRE(Field<uint32_t>(material.bytes, start) == 1);
    REQUIRE(Field<uint32_t>(material.bytes, start + 4) == 10);
    material.settings = start + 8;
    material.texture = Field<uint32_t>(material.bytes, material.settings + 30 + 4 + 16 + 16 + 16);
    return material;
}
void Fixture(const std::vector<uint8_t>& elf, const std::vector<uint8_t>& model,
             const Material& material, const std::vector<uint8_t>& texture, unsigned variant) {
    std::memset(ram::bytes.data(), 0, ram::bytes.size());
    Elf(elf);
    if (!elf_image) {
        elf_image = std::make_unique<decltype(ram::bytes)>();
        std::memcpy(elf_image->data(), ram::bytes.data(), ram::bytes.size());
        if (artifacts.Enabled())
            for (bool native : {false, true}) {
                auto& out = artifacts.Input(native);
                Word(out, uint32_t(0x46454c45));
                out.insert(out.end(), elf_image->begin(), elf_image->end());
            }
    }
    sky_content_oracle::allocation = 0x500000;
    std::fesetround(FE_TOWARDZERO);
    Call(0x1cc580); // actual complete program registration, including type A
    Call(0x1da2d8); // actual source alpha presets
    // Pre-existing initialized software allocation pool; cold pool creation is
    // deliberately outside this loaded-resource producer fixture.
    ram::write<uint32_t>(0x3114f0 - 0x70b0, 1);
    ram::write<uint8_t>(0x3114f0 - 0x7d6f, 1);
    ram::write<uint32_t>(title::MaterialState::kDisplayWidth, 512);
    ram::write<uint32_t>(title::MaterialState::kDisplayHeight, 448);
    ram::write<uint32_t>(title::MaterialState::kDisplayPixelBytes, 2);
    ram::write<uint32_t>(title::MaterialState::kDepthBufferPage, 168);
    ram::write(title::MaterialState::kRenderTarget, kTarget);
    ram::write(title::SkySetup::kRenderView, kView);
    ram::write(kTarget, std::array<uint32_t,8>{0,0,0,512,448,0,512,448});
    const float angle = variant * 0.37f, c = std::cos(angle) * 0.013f, s = std::sin(angle) * 0.013f;
    const Matrix clip{{{c,0,s,0},{0,0.014f,0,0},{-s,0,c,0.003f},{0,0,0,1}}};
    ram::write(kView + 0x80, clip); ram::write(kClip, clip);
    ram::write(kView + 0x140, std::array<float,4>{1.1f,1.1f,1.1f,1});
    ram::write(kView + 0x160, kReference); ram::write(kReference, kReference + 16);
    ram::write(kSky + 8, uint32_t(1)); ram::write(kSky + 12, kList); ram::write(kList, kRigid);
    ram::write(kRigid + 8, kList + 0x100); ram::write(kRigid + 12, kModel);
    const uint32_t parts = Field<uint32_t>(model, 0);
    REQUIRE(parts <= 64);
    ram::write(kRigid + 0x18, parts);
    ram::write(kModel + 8, parts);
    ram::write(kModel + 12, kList + 0x300);
    ram::write(kModel + 16, kList + 0x400);
    ram::write(kModel + 24, kList + 0x500);
    uint32_t data = 0x600000;
    size_t cursor = 4;
    for (uint32_t i = 0; i < parts; ++i) {
        const uint32_t count = Field<uint32_t>(model, cursor), size = Field<uint32_t>(model, cursor + 4);
        cursor += 8;
        REQUIRE(size <= model.size() - cursor);
        std::memcpy(ram::bytes.data() + ram::Address(data, size), model.data() + cursor, size);
        Disk(int32_t(i), data, size);
        ram::write(kList + 0x300 + i * 4, count); ram::write(kList + 0x400 + i * 4, size);
        ram::write(kList + 0x500 + i * 4, i);
        ram::write(kList + 0x100 + i * 4, kList + 0x200);
        data += (size + 15) & ~15u; cursor += size;
        cursor += 4 + Field<uint32_t>(model, cursor);
    }
    REQUIRE(cursor == model.size());
    ram::write(kList + 0x208, kMaterial);
    ram::write(kMaterial, kShader); ram::write<uint32_t>(kMaterial + 16, 1);
    ram::write(kMaterial + 24, Field<uint64_t>(material.bytes, 0));
    ram::write<uint32_t>(kMaterial + 32, 0);
    constexpr unsigned shift[]{0,1,5,6,9,17,19,20,21,23,26,27,28,29,30,
                                32,35,38,39,41,43,45,47,55,56,57,58,59,60,61};
    uint64_t settings = 0;
    for (size_t i = 0; i != 30; ++i) settings |= uint64_t(material.bytes[material.settings + i]) << shift[i];
    ram::write(kShader, settings);
    ram::write(kShader + 0x14, kTexture);
    ram::write(kShader + 0x18, Field<uint32_t>(material.bytes, material.settings + 30));
    ram::write(kShader + 0x50, std::array<float,2>{variant * 0.11f, -float(variant) * 0.19f});
    ram::write<uint32_t>(kShader + 0x64, 10);
    ram::write<uint32_t>(kShader + 0x6c, title::SkyMaterial::kVtable);
    REQUIRE(ram::read<uint32_t>(title::SkyMaterial::kVtable + 0x14) == 0x1d9268);
    REQUIRE(ram::read<uint32_t>(title::SkyMaterial::kVtable + 0x34) == 0x1d85a0);
    REQUIRE(texture.size() > 132);
    ram::write(kTexture + 8, Field<uint32_t>(texture, 0));
    std::memcpy(ram::bytes.data() + kTexture + 12, texture.data() + 4, 96);
    ram::write<int32_t>(kTexture + 0x60, 100);
    ram::write<uint32_t>(kTexture + 0x64, 0x1d000);
    ram::write<uint16_t>(kTexture + 0x6a, 0);
    ram::write(0x1d000, std::array<uint32_t,4>{0x3000,0,0,kTexture+12});
    const uint32_t storage_size = uint32_t(texture.size() - 132);
    Disk(100, 0x680000, storage_size);
    std::memcpy(ram::bytes.data() + 0x680000, texture.data() + 132, storage_size);
    Call(0x1d81e0, kShader); // actual generated settings producer
    if (variant % 3 == 1) {
        ram::write<uint8_t>(kMaterial + 0x64,1);
        ram::write<uint32_t>(Pass::kMaterialCount,1);
        ram::write(Pass::kMaterials,kMaterial);
    }
    ram::write(kBucket + 0x10, kRoot);
    ram::write<uint8_t>(kBucket + 0x15, 1); // first program region
    ram::write<uint32_t>(kRoot, 0x20000000);
    ram::write(0x323310, kPacket);
}
struct Sink : ps2r::gs::DrawIrSink {
    std::vector<Draw> draws;
    void OnDraw(const Draw& draw) override { draws.push_back(draw); }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};
struct ModelRenderer {
    MainMemory memory;
    Scratchpad scratch;
    Sink sink;
    ps2r::gs::GsState gs;
    GsStateSink gs_sink{gs};
    GifRegs regs;
    GifParser parser{gs_sink,regs};
    GifArbiter arbiter{parser};
    VuUnit vu0{0}, vu1{1,&arbiter};
    Vif vif{vu0,vu1,arbiter,regs};
    ChainWalker walker{memory,scratch,vif,arbiter};
    void Render() {
        std::memcpy(memory.Data(), ram::bytes.data(), ram::bytes.size());
        gs.SetSink(&sink);
        const auto env = Environment();
        gs.WriteRegister(0x46,env.colour_clamp);
        gs.WriteRegister(0x49,env.per_pixel_alpha);
        gs.WriteRegister(0x22,env.scan_mask);
        gs.WriteRegister(0x44,env.dither_matrix);
        gs.WriteRegister(0x3d,uint64_t(env.fog_r) | uint64_t(env.fog_g) << 8 | uint64_t(env.fog_b) << 16);
        vu1.dispatch().settings().vu1_mode = VuTierMode::Interp;
        DmacChannelRegs channel{};
        channel.chcr = DmacChcr::kStr | DmacChcr::kTte | (DmacChcr::kModChain << 2);
        channel.tadr = kRoot;
        const auto result = walker.WalkVifChain(1,channel);
        REQUIRE(result.completed && result.tag_count < ChainWalker::kMaxTags);
        REQUIRE(vif.IsIdle(1));
        arbiter.drain(); gs.KickBoundary();
    }
    std::vector<uint8_t> Micro() const { return {vu1.state().micro, vu1.state().micro + Vu1State::kMemSize}; }
};
// The VU1 program residency after the pass (SkyPrograms): the four globals,
// the sky bucket's last key / last call, the type-A program's table entry, the
// MPG uploads on the sky bucket's chain and -- walked through the real
// DMAC/VIF -- the VU1 micro memory those uploads leave behind.
struct Residency {
    std::array<uint32_t,4> globals{};        // time, bucket, next, start
    uint64_t last_key = 0;
    uint32_t last_call = 0;
    std::array<uint32_t,3> program{};        // address, loaded, bucketAddresses[0]
    std::vector<std::array<uint32_t,3>> uploads;   // REF qwc|tag, source, MPG vif word
    std::vector<uint8_t> micro;
};
struct Result {
    std::vector<Draw> draws;
    Matrix matrix;
    std::array<uint8_t,3> flags;
    uint32_t listed = 0;
    Residency residency;
};
// Walk the bucket's chain from its first tag, collecting the MPG REF tags.
std::vector<std::array<uint32_t,3>> MpgUploads(uint32_t tag) {
    std::vector<std::array<uint32_t,3>> out;
    for (size_t steps = 0; steps < 4096 && tag && tag < ram::bytes.size() - 16; ++steps) {
        const auto words = ram::read<std::array<uint32_t,4>>(tag);
        const uint32_t id = (words[0] >> 28) & 7, qwc = words[0] & 0xffff;
        if (id == 3 && (words[3] & 0xff000000u) == title::SkyPrograms::kVifMpg) out.push_back({words[0], words[1], words[3]});
        if (id == 1) tag += 16 + qwc * 16;             // CNT: data follows, next tag after it
        else if (id == 2) { if (words[1] == tag) break; tag = words[1]; }   // NEXT: sealed when it names itself
        else if (id == 3) tag += 16;                   // REF: data elsewhere, next tag follows
        else break;                                    // END (7) / anything else
    }
    return out;
}
Residency ReadResidency() {
    using P = title::SkyPrograms;
    Residency r;
    r.globals = {ram::read<uint32_t>(P::kTime), ram::read<uint32_t>(P::kBucket),
                 ram::read<uint32_t>(P::kNext), ram::read<uint32_t>(P::kStart)};
    r.last_key = ram::read<uint64_t>(kBucket + P::kBucketLastKey);
    r.last_call = ram::read<uint32_t>(kBucket + P::kBucketLastCall);
    const uint32_t entry = P::kPrograms + ram::read<uint32_t>(P::kTypeAProgram) * P::kProgramStride;
    r.program = {ram::read<uint32_t>(entry + P::kProgramAddress), ram::read<uint32_t>(entry + P::kProgramLoaded),
                 uint32_t(ram::read<uint16_t>(entry + P::kProgramBucketAddresses))};
    r.uploads = MpgUploads(kRoot);
    return r;
}
void RenderOwned(const Pass::Output& output) {
    // Real owned allocations and ordered upload/draw delivery. This backend
    // records calls; pixel acceptance remains the parent's rendering gate.
    struct Backend : ps2r::test::FakeBackend {
        uint32_t sky_color = 0;
        size_t geometry = 0, composites = 0;
        ps2r::render::DrawResult Draw(const ::Draw& draw, const ps2r::gs::TextureSource& texture,
                                      const ps2r::render::DrawParams& params) override {
            REQUIRE(params.inherited_uniforms == 0 && params.cd_copy == nullptr);
            REQUIRE(!params.do_frame_clear && !params.zsrc_armed);
            REQUIRE(draw.raster.frame.fbp == 0 && draw.raster.zbuf.zbp == 0);
            REQUIRE(draw.tex.tbp == 0 && draw.tex.cbp == 0);
            const auto& color = At(params.color).desc;
            const auto& depth = At(params.depth).desc;
            REQUIRE(color.width == depth.width && color.height == depth.height && color.scale == depth.scale);
            if (draw.ctx == 0) {
                REQUIRE(!composites);
                if (!sky_color) sky_color = params.color.id;
                REQUIRE(sky_color == params.color.id);
                REQUIRE(texture.owner.kind == ps2r::gs::TextureSource::OwnerKind::Decoded);
                REQUIRE(texture.owner.levels == 0 && texture.transform.mip_levels == 0);
                REQUIRE(texture.transform.mip_k == draw.tex.k && texture.transform.mip_l == draw.tex.l);
                REQUIRE(texture.sampler.mag_linear == draw.tex.mmag && texture.sampler.mmin == draw.tex.mmin);
                REQUIRE(texture.sampler.wms == 0 && texture.sampler.wmt == 0 && !texture.transform.uv_native);
                REQUIRE(params.render_w == 256 && params.render_h == 224);
                REQUIRE(!At(texture.owner.surface).rgba.empty());
                ++geometry;
            } else {
                REQUIRE(texture.owner.kind == ps2r::gs::TextureSource::OwnerKind::Surface);
                REQUIRE(texture.owner.surface.id == sky_color || !geometry);
                REQUIRE(params.color.id != texture.owner.surface.id && params.render_w == 512);
                REQUIRE(!draw.raster.zbuf.zmsk && draw.raster.test.ztst == 1);
                ++composites;
            }
            return FakeBackend::Draw(draw, texture, params);
        }
    } backend;
    Resources resources(backend);
    Graphics queue;
    Targets targets(queue);
    title::GraphicsRenderer renderer(backend, resources);
    using Format = ps2r::render::SurfaceFormat;
    const uint8_t scale = uint8_t(1 + cases % 2);
    for (auto name : {Targets::Name::SceneColor, Targets::Name::SceneDepth, Targets::Name::SceneCopy,
                      Targets::Name::SkyColor, Targets::Name::SkyDepth, Targets::Name::SkyCopy}) {
        const bool sky = name >= Targets::Name::SkyColor;
        const bool depth = name == Targets::Name::SceneDepth || name == Targets::Name::SkyDepth;
        Graphics::Handle handle;
        REQUIRE(resources.Register(queue, {uint32_t((sky ? 256 : 512)*scale),uint32_t((sky ? 224 : 448)*scale),
            depth ? Format::Depth32f : Format::Rgba8,!depth,scale}, &handle) == Resources::Result::Ok);
        REQUIRE(targets.Bind(name,handle) == Targets::Result::Ok);
    }
    REQUIRE(queue.BeginFrame(0) == Graphics::Result::Ok);
    std::map<Assets::TextureLease,Graphics::Handle> images;
    for (const auto& draw : output.draws) {
        auto found = images.find(draw.texture);
        if (found == images.end()) {
            const auto& image = *draw.texture;
            Graphics::Handle handle;
            REQUIRE(resources.Register(queue,{image.width,image.height,Format::Rgba8,false,1},&handle) == Resources::Result::Ok);
            REQUIRE(queue.RecordUpload(queue.End(0),handle,{0,0,int(image.width),int(image.height)},
                                       image.width*4,image.rgba) == Graphics::Result::Ok);
            found = images.emplace(draw.texture,handle).first;
        }
        REQUIRE(targets.RecordSky(queue.End(0),draw.intent,found->second) == Targets::Result::Ok);
    }
    REQUIRE(targets.Record(queue.End(0),output.finish.draw,Targets::Name::SkyColor) == Targets::Result::Ok);
    // In-flight uploads and draws pin a version even after the source owner
    // releases it. Rebinding the object must never redirect old work.
    for (const auto& image : images) REQUIRE(queue.ReleaseResource(image.second) == Graphics::Result::Ok);
    Graphics::Hooks hooks;
    hooks.upload = [&](const Graphics::Upload& upload) { return resources.Upload(upload) == Resources::Result::Ok; };
    hooks.draw = [&](const Draw& draw,const std::vector<Graphics::Lease>& bindings) {
        return renderer.Draw(draw,bindings) == title::GraphicsRenderer::Result::Ok;
    };
    hooks.drain = [] { return true; };
    REQUIRE(queue.Submit(hooks) == Graphics::Result::Ok);
    REQUIRE(backend.geometry == output.draws.size() && backend.composites == 1);
    REQUIRE(backend.readbacks == 0 && backend.clears == 0 && backend.level_uploads == 0);
    REQUIRE(queue.Counts().replaced_bucket_jobs[0] == output.draws.size() + 1 + images.size());
}
Result Run(bool selected) {
    std::map<uint32_t, Pass::Body> dispatch;
    Pass::Hooks hooks;
    hooks.enabled = selected;
    hooks.read = Reader();
    hooks.environment = [](auto& env) { env = Environment(); return true; };
    hooks.write = [](const std::vector<Pass::Write>& writes) {
        for (const auto& write : writes)
            if (write.address > ram::bytes.size() || write.bytes.size() > ram::bytes.size() - write.address) return false;
        for (const auto& write : writes)
            std::memcpy(ram::bytes.data() + write.address, write.bytes.data(), write.bytes.size());
        return true;
    };
    hooks.begin = [](uint32_t w, uint32_t h) { return w == 512 && h == 448; };
    Result result;
    hooks.record = [&](const Pass::Output& output) {
        for (const auto& draw : output.draws) {
            REQUIRE(draw.texture && !draw.texture->rgba.empty());
            result.draws.push_back(draw.intent);
        }
        result.draws.push_back(output.finish.draw);
        RenderOwned(output);
        return true;
    };
    Pass pass(std::move(hooks));
    REQUIRE(pass.Install([&](uint32_t address) -> Pass::Body {
        // Readers themselves are retained; the fixture supplies their completed
        // software result. No draw/setting/program producer is substituted.
        return [address](CpuContext& c, uint32_t at) {
            if (address == Assets::kModelRead || address == Assets::kTextureRead) return;
            REQUIRE(at == address);
            ++model_bodies;
            sky_content_oracle::Run(address, c);
        };
    }, [&](uint32_t entry, Pass::Body body) { dispatch[entry] = std::move(body); return true; }));
    const auto call = [&](uint32_t entry, uint32_t a0, uint32_t a1=0, uint32_t a2=0, uint32_t a3=0) {
        auto context = Context();
        context.cpuRegs.GPR.r[4].UL[0]=a0; context.cpuRegs.GPR.r[5].UL[0]=a1;
        context.cpuRegs.GPR.r[6].UL[0]=a2; context.cpuRegs.GPR.r[7].UL[0]=a3;
        dispatch.at(entry)(context,entry);
    };
    call(Assets::kTextureRead,kTexture); call(Assets::kModelRead,kModel);
    if (selected) {
        REQUIRE(pass.Stats().models_loaded == 1 && pass.Stats().textures_loaded == 1);
        // Every actual NEW model/selected case now renders from the persisted
        // typed allocations. No generated producer or source arithmetic changes.
        Assets::Snapshot snapshot, restored;
        std::vector<uint8_t> bytes, again;
        REQUIRE(pass.CaptureAssets(snapshot));
        REQUIRE(Assets::Preflight(snapshot, Reader()));
        REQUIRE(title::SkyState::Encode(snapshot, bytes));
        REQUIRE(title::SkyState::Decode(bytes, restored));
        REQUIRE(title::SkyState::Encode(restored, again) && bytes == again);
        pass.Reset();
        REQUIRE(pass.RestoreAssets(restored));
    }
    // The frame's first-bucket work before the sky, on both arms: the resident
    // program uploads and the shared dispatcher inputs (actual generated
    // producers). The owned pass appends its program upload behind them, so
    // the walked micro image is the frame's.
    Call(0x1da060); // resident uploads: actual generated producers
    Call(0x1bbec0); // shared dispatcher inputs: actual generated producer
    const size_t resident_uploads = MpgUploads(kRoot).size();
    call(title::SkySetup::kAddress,kSky);
    result.matrix = ram::read<Matrix>(kSky + 0x10);
    ram::write(kScreen,result.matrix);
    call(Pass::kDrawModel,kRigid,kView,kScreen,kClip);
    call(title::SkyIntent::kAddress,kSky);
    result.flags = ram::read<std::array<uint8_t,3>>(kMaterial + 0x64);
    result.listed = ram::read<uint32_t>(Pass::kMaterialCount);
    result.residency = ReadResidency();
    if (selected) {
        REQUIRE(pass.Stats().start.replaced == 1 && pass.Stats().model.replaced == 1 &&
                pass.Stats().finish.replaced == 1 && !pass.Stats().faults);
        REQUIRE(pass.Stats().start.model_calls == 0 && pass.Stats().model.model_calls == 0 &&
                pass.Stats().finish.model_calls == 0);
        // The owned pass left the program uploads (and nothing else) on the sky
        // bucket's chain: the real DMAC/VIF walk of that chain is the residency
        // the model's chain produces in VU1 micro memory.
        REQUIRE(result.residency.uploads.size() >= resident_uploads &&
                pass.Stats().programs_uploaded == result.residency.uploads.size() - resident_uploads);
        const uint32_t tail = ram::read<uint32_t>(kBucket + 0x10);
        ram::write<uint32_t>(tail, 0x70000000);
        auto renderer = std::make_unique<ModelRenderer>();
        renderer->Render();
        REQUIRE(renderer->sink.draws.empty());
        result.residency.micro = renderer->Micro();
    } else {
        const uint32_t tail = ram::read<uint32_t>(kBucket + 0x10);
        ram::write<uint32_t>(tail, 0x70000000); // source bucket sealing, no payload rewrite
        auto renderer = std::make_unique<ModelRenderer>();
        renderer->Render();
        result.draws = std::move(renderer->sink.draws);
        result.residency.micro = renderer->Micro();
    }
    Accumulate(selected, pass.Stats());
    if (artifacts.Enabled()) {
        auto& out = artifacts.Output(selected, "draw");
        Word(out, uint32_t(result.draws.size()));
        for (auto draw : result.draws) {
            // The compared canonical form: model job numbering/epochs and
            // VRAM-address resource identities have no renderer meaning in the
            // owned arm (Normalize below); inactive ALPHA and MXL=0 mips zeroed.
            draw.seq = draw.vram_epoch = 0;
            draw.tex.tbp = draw.tex.cbp = 0;
            artifacts.RecordDraw(selected, draw, true, false);
        }
        auto& state = artifacts.Output(selected, "state");
        state.insert(state.end(), reinterpret_cast<const uint8_t*>(result.matrix.data()),
                     reinterpret_cast<const uint8_t*>(result.matrix.data()) + sizeof(Matrix));
        state.insert(state.end(), result.flags.begin(), result.flags.end());
        Word(state, result.listed);
    }
    return result;
}
void Normalize(Draw& draw) {
    // Model job numbering/upload epochs have no renderer meaning in the
    // selected path. Preserve actual draw order with the compared row index.
    draw.seq = draw.vram_epoch = 0;
    // Resource identities are owned bindings in the selected arm, never VRAM
    // addresses. MIP state is inactive in type A and FinishSky (MMIN < 2).
    draw.tex.tbp = draw.tex.cbp = 0;
    for (size_t i = 0; i != 6; ++i) draw.tex.mip_tbp[i] = draw.tex.mip_tbw[i] = 0;
    if (!draw.alpha_blend) draw.raster.alpha = {};
}
void Compare(Result model, Result selected) {
    REQUIRE(std::memcmp(model.matrix.data(), selected.matrix.data(), sizeof(Matrix)) == 0);
    REQUIRE(model.flags == selected.flags && model.listed == selected.listed);
    // VU1 program residency (SkyPrograms): the model arm loaded the type-A
    // program through RenderSkyMaterial; the owned arm must leave the same
    // tables, the same MPG uploads and the same VU1 micro image.
    const auto& a = model.residency; const auto& b = selected.residency;
    if (a.globals != b.globals || a.last_key != b.last_key || a.last_call != b.last_call || a.program != b.program ||
        a.uploads != b.uploads || a.micro != b.micro) {
        std::fprintf(stderr, "residency model/selected: time %u/%u bucket %u/%u next %u/%u start %u/%u key %llx/%llx call %u/%u "
                     "program %u,%u,%u/%u,%u,%u uploads %zu/%zu micro %s\n",
                     a.globals[0], b.globals[0], a.globals[1], b.globals[1], a.globals[2], b.globals[2], a.globals[3], b.globals[3],
                     (unsigned long long)a.last_key, (unsigned long long)b.last_key, a.last_call, b.last_call,
                     a.program[0], a.program[1], a.program[2], b.program[0], b.program[1], b.program[2],
                     a.uploads.size(), b.uploads.size(), a.micro == b.micro ? "same" : "differs");
        for (size_t i = 0; i < std::max(a.uploads.size(), b.uploads.size()); ++i) {
            const auto u = i < a.uploads.size() ? a.uploads[i] : std::array<uint32_t,3>{};
            const auto v = i < b.uploads.size() ? b.uploads[i] : std::array<uint32_t,3>{};
            std::fprintf(stderr, "  upload%zu model %08x %08x %08x selected %08x %08x %08x\n", i, u[0], u[1], u[2], v[0], v[1], v[2]);
        }
    }
    REQUIRE(a.globals == b.globals && a.last_key == b.last_key && a.last_call == b.last_call);
    REQUIRE(a.program == b.program);
    REQUIRE(!a.uploads.empty() && a.uploads == b.uploads);
    REQUIRE(a.micro.size() == b.micro.size() && a.micro == b.micro);
    residency_uploads += a.uploads.size();
    if (model.draws.size() != selected.draws.size())
        std::fprintf(stderr,"draws model%zu selected%zu\n",model.draws.size(),selected.draws.size());
    REQUIRE(model.draws.size() == selected.draws.size());
    for (size_t i=0;i<model.draws.size();++i) {
        auto& a=model.draws[i]; auto& b=selected.draws[i];
        Normalize(a); Normalize(b);
        const auto ar=a.ToDrawRec(), br=b.ToDrawRec();
        if (std::memcmp(&ar,&br,sizeof(ar))) {
            const auto* x=reinterpret_cast<const uint8_t*>(&ar); const auto* y=reinterpret_cast<const uint8_t*>(&br);
            std::fprintf(stderr,"draw%zu types%u/%u",i,a.prim_type,b.prim_type);
            for(size_t j=0;j<sizeof(ar);++j) if(x[j]!=y[j]) std::fprintf(stderr," [%zu]%02x/%02x",j,x[j],y[j]);
            std::fprintf(stderr,"\n"); REQUIRE(false);
        }
        REQUIRE(a.verts.size()==b.verts.size());
        REQUIRE(std::memcmp(a.verts.data(),b.verts.data(),a.verts.size()*sizeof(a.verts[0]))==0);
        REQUIRE(a.raster.dimx==b.raster.dimx && a.raster.dthe==b.raster.dthe);
        REQUIRE(a.tex.mxl==b.tex.mxl && a.tex.k==b.tex.k && a.tex.l==b.tex.l && a.tex.lcm==b.tex.lcm);
        vertices_compared+=a.verts.size();
        if (comparison) comparison<<cases<<'\t'<<i<<'\t'<<unsigned(a.prim_type)<<'\t'<<a.verts.size()<<'\t'
            <<std::hex<<a.snapshot.vertex_hash<<'\t'<<b.snapshot.vertex_hash<<std::dec<<"\t0\n";
    }
    ++cases;
}
void AssetLifetimes() {
    Assets assets;
    REQUIRE(assets.ModelLoaded(Reader(),kModel)==Assets::Result::Ok);
    REQUIRE(assets.TextureLoaded(Reader(),kTexture)==Assets::Result::Ok);
    const auto first_model=assets.FindModel(kModel);
    const auto first_texture=assets.FindTexture(kTexture);
    REQUIRE(first_model && first_texture);
    const auto pixels=first_texture->rgba;
    const auto vertex=first_model->parts[0].strips[0].vertices[0];
    // Destruction/source compaction cannot revoke already owned work.
    assets.ForgetModel(kModel); assets.ForgetTexture(kTexture);
    REQUIRE(!assets.FindModel(kModel) && !assets.FindTexture(kTexture));
    REQUIRE(first_texture->rgba==pixels && first_model->parts[0].strips[0].vertices[0].position==vertex.position);
    REQUIRE(assets.ModelLoaded(Reader(),kModel)==Assets::Result::Ok);
    REQUIRE(assets.TextureLoaded(Reader(),kTexture)==Assets::Result::Ok);
    REQUIRE(assets.FindModel(kModel)!=first_model && assets.FindTexture(kTexture)!=first_texture);
    // A failed reload must invalidate the current version, not reuse it.
    const uint16_t width=ram::read<uint16_t>(kTexture+16);
    ram::write<uint16_t>(kTexture+16,14);
    REQUIRE(assets.TextureLoaded(Reader(),kTexture)==Assets::Result::UnsupportedAsset);
    REQUIRE(!assets.FindTexture(kTexture) && first_texture->rgba==pixels);
    ram::write(kTexture+16,width);
    const uint32_t size=ram::read<uint32_t>(kList+0x400);
    ram::write<uint32_t>(kList+0x400,0xfffffff0);
    REQUIRE(assets.ModelLoaded(Reader(),kModel)!=Assets::Result::Ok);
    REQUIRE(!assets.FindModel(kModel) && first_model->parts[0].strips[0].vertices[0].position==vertex.position);
    ram::write(kList+0x400,size);
    REQUIRE(assets.ModelLoaded(Reader(),kModel)==Assets::Result::Ok);
    REQUIRE(assets.TextureLoaded(Reader(),kTexture)==Assets::Result::Ok);
    assets.Reset();
    REQUIRE(!assets.FindModel(kModel) && !assets.FindTexture(kTexture) && first_texture->rgba==pixels);
}
}
int main(int argc,char** argv) {
    if(argc<3 || !std::filesystem::is_directory(argv[2])) return 77;
    const auto elf=File(argv[1]); if(elf.empty()) return 77;
    // elf assets [comparison.tsv] [--export-dir DIR --source-manifest FILE]
    const bool tsv = argc >= 4 && std::strncmp(argv[3], "--", 2) != 0;
    if(tsv) comparison.open(argv[3]);
    REQUIRE(artifacts.Parse(argc, argv, tsv ? 4 : 3));
    std::vector<std::filesystem::path> models, materials;
    std::map<uint32_t,std::filesystem::path> textures;
    for(const auto& entry:std::filesystem::directory_iterator(argv[2])) {
        const auto name=entry.path().filename().string();
        if(name.find("model_")==0) models.push_back(entry.path());
        if(name.find("material_")==0) materials.push_back(entry.path());
        if(name.find("texture_")==0) textures.emplace(uint32_t(std::stoul(name.substr(8,8),nullptr,16)),entry.path());
    }
    std::sort(models.begin(),models.end()); std::sort(materials.begin(),materials.end());
    REQUIRE(!models.empty() && !materials.empty());
    const int saved=std::fegetround();
    for(size_t i=0;i<materials.size();++i) {
        const auto material=ReadMaterial(materials[i]);
        const auto model=File(models[i%models.size()]), texture=File(textures.at(material.texture));
        Fixture(elf,model,material,texture,unsigned(i%8));
        if(i==0) AssetLifetimes();
        auto input=std::make_unique<decltype(ram::bytes)>();
        std::memcpy(input->data(),ram::bytes.data(),ram::bytes.size());
        if (artifacts.Enabled()) { RecordInput(false); RecordInput(true); }
        const auto expected=Run(false);
        std::memcpy(ram::bytes.data(),input->data(),ram::bytes.size());
        constexpr int modes[]{FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD};
        std::fesetround(modes[i%4]);
        const auto selected=Run(true);
        REQUIRE(std::fegetround()==modes[i%4]);
        Compare(expected,selected);
    }
    std::fesetround(saved);
    std::printf("SkyPass NEW generated self-oracle: %zu cases, %zu vertices, %zu model boundary calls; zero active DrawIR/vertex differences; "
                "%zu VU1 program uploads, residency (tables, chain MPG tags, micro image) identical on every case\n",
                cases,vertices_compared,model_bodies,residency_uploads);
    if (artifacts.Enabled()) {
        for (bool native : {false,true}) {
            const auto& c = census[native];
            artifacts.Counter(native, "graphics_sky_start", Row(c.start));
            artifacts.Counter(native, "graphics_sky_model", Row(c.model));
            artifacts.Counter(native, "graphics_sky_finish", Row(c.finish));
            artifacts.Counter(native, "graphics_sky_assets", "models_loaded=" + std::to_string(c.models_loaded) +
                ",textures_loaded=" + std::to_string(c.textures_loaded) + ",load_declines=" + std::to_string(c.load_declines) +
                ",faults=" + std::to_string(c.faults));
            // Owned Sky passes delivered through Graphics (start+model+finish replaced together).
            artifacts.Counter(native, "graphics_sky_output", "completed=" + std::to_string(c.finish.replaced));
        }
        artifacts.Setting("hle.graphics_sky", "off", "on");
        artifacts.Setting("assets", "static extracted retail Sky assets (ELF + model/material/texture files)",
                          "static extracted retail Sky assets (ELF + model/material/texture files)");
        artifacts.Note("scope", "source StartSkyBuffer / sky model draw / FinishSky: every draw of the pass (type-A "
            "geometry and the 32-pixel copy-out) as canonical DrawRec + out-of-record fields + vertices, plus the "
            "source-visible state (sky matrix, material flags, listed material count); no pixel claim");
        artifacts.Note("canonicalization", "seq/vram_epoch zeroed (model job numbering), tex.tbp/cbp zeroed (VRAM "
            "addresses are owned bindings in the native arm), inactive ALPHA and unaddressed (MXL=0 or MMIN<2) mip addresses zeroed; identical "
            "on both arms; vertices raw");
        artifacts.Note("input", "the loaded ELF image once per arm, then per case the 4 KiB pages the fixture changed "
            "relative to it, captured before the model run and restored before the native run");
        artifacts.Note("rounding", "the native arm cycles the four host modes per case (the test asserts equality under "
            "each); not a receipt setting");
        artifacts.Note("counter_provenance", "graphics_sky_*: SkyPass::Run census, published by "
            "hle/graphics/graphics_report.cpp; graphics_sky_output.completed = owned passes whose FinishSky was replaced");
        artifacts.Finish("SkyPassSelfOracle", "sky-pass-artifacts-v1", argc, argv);
    }
}
