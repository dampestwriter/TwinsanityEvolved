// Complete NEW source producers + unchanged NEW transport/shaders versus the
// selected source adapters, on identical ELF/data/objects. No game execution.
#include "games/slus_20909_crash_twinsanity/tests/recomp/rigid_content_oracle_memory.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_pass.h"
#include "tests/lib/fake_backend.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON
namespace rigid_content_oracle {
namespace memory { std::array<uint8_t, 0x800000> bytes{}; }
uint32_t allocation = 0x80000;
std::map<uint32_t, title::MeshPass::Body> overrides;
void Run(uint32_t address, CpuContext& context) {
    const auto found = overrides.find(address);
    if (found != overrides.end()) found->second(context, address);
    else Original(address, context);
}
void External(uint32_t address, CpuContext& context) {
    const bool aligned = address == 0x204530;
    const uint32_t size = context.cpuRegs.GPR.r[aligned ? 5 : 4].UL[0];
    const uint32_t alignment = aligned ? context.cpuRegs.GPR.r[6].UL[0] : 16;
    if ((address != 0x1815c0 && !aligned) || size > 65536 || !alignment ||
        alignment > 128 || (alignment & (alignment - 1))) {
        std::fprintf(stderr, "Unexpected generated external %08x\n", address); std::abort();
    }
    allocation = (allocation + alignment - 1) & ~(alignment - 1);
    context.cpuRegs.GPR.r[2].UL[0] = allocation;
    allocation += (size + 15) & ~15u;

}
}
namespace {
namespace ram = rigid_content_oracle::memory;
using Pass = title::MeshPass;
using Assets = title::MeshAssets;
using Draw = ps2r::gs::DrawIr;
using Matrix = title::MeshGeometry::Matrix;
using Graphics = title::Graphics;
using Targets = title::GraphicsTargets;
using Resources = title::GraphicsResources;
constexpr uint32_t kSky = 0x10000, kRigid = 0x11000, kModel = 0x12000;
constexpr uint32_t kMaterial = 0x13000, kShader = 0x14000, kTexture = 0x15000;
constexpr uint32_t kTarget = 0x16000, kView = 0x17000, kReference = 0x18000;
constexpr uint32_t kScreen = 0x19000, kClip = 0x1a000, kList = 0x1b000;
constexpr uint32_t kRoot = 0x20000, kPacket = 0x23000, kBucket = 0x323658;
size_t cases = 0, vertices_compared = 0, model_bodies = 0;
std::ofstream comparison;
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"MeshPass case%zu line%d: %s\n",cases,__LINE__,#x); std::exit(1); } } while(false)
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
    rigid_content_oracle::Run(entry, context);
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
    REQUIRE(Field<uint32_t>(material.bytes, start + 4) == 1);
    material.settings = start + 8;
    material.texture = Field<uint32_t>(material.bytes, material.settings + 30 + 4 + 16 + 16 + 16);
    return material;
}
void Fixture(const std::vector<uint8_t>& elf, const std::vector<uint8_t>& model,
             const Material& material, const std::vector<uint8_t>& texture, unsigned variant) {
    std::memset(ram::bytes.data(), 0, ram::bytes.size());
    Elf(elf);
    rigid_content_oracle::allocation = 0x80000;
    rigid_content_oracle::overrides.clear();
    std::fesetround(FE_TOWARDZERO);
    Call(0x1cc580); // actual complete program registration, including type A
    Call(0x1da2d8); // actual source alpha presets
    // Pre-existing initialized software allocation pool; cold pool creation is
    // deliberately outside this loaded-resource producer fixture.
    ram::write<uint32_t>(0x3114f0 - 0x70b0, 1);
    ram::write<uint32_t>(0x3114f0 - 0x70b4, 1);
    ram::write<uint8_t>(0x3114f0 - 0x7d70, 1);
    ram::write<uint8_t>(0x3114f0 - 0x7d6f, 1);
    ram::write<uint32_t>(title::MaterialState::kDisplayWidth, 512);
    ram::write<uint32_t>(title::MaterialState::kDisplayHeight, 448);
    ram::write<uint32_t>(title::MaterialState::kDisplayPixelBytes, 2);
    ram::write<uint32_t>(title::MaterialState::kDepthBufferPage, 168);
    ram::write(title::MaterialState::kRenderTarget, kTarget);
    ram::write(title::MeshIntent::kRenderView, kView);
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
    ram::write<uint32_t>(kMaterial + 32, Field<uint32_t>(material.bytes, 8));
    ram::write<uint32_t>(kMaterial + 0x60, 1);
    constexpr unsigned shift[]{0,1,5,6,9,17,19,20,21,23,26,27,28,29,30,
                                32,35,38,39,41,43,45,47,55,56,57,58,59,60,61};
    uint64_t settings = 0;
    for (size_t i = 0; i != 30; ++i) settings |= uint64_t(material.bytes[material.settings + i]) << shift[i];
    ram::write(kShader, settings);
    ram::write(kShader + 0x14, kTexture);
    ram::write(kShader + 0x18, Field<uint32_t>(material.bytes, material.settings + 30));
    ram::write(kShader + 0x50, std::array<float,2>{variant * 0.11f, -float(variant) * 0.19f});
    ram::write<uint32_t>(kShader + 0x64, 1);
    ram::write<uint32_t>(kShader + 0x6c, title::MeshMaterial::kVtable);
    REQUIRE(ram::read<uint32_t>(title::MeshMaterial::kVtable + 0x14) == 0x1d8ae0);
    REQUIRE(ram::read<uint32_t>(title::MeshMaterial::kVtable + 0x34) == 0x1d49c8);
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
    Call(0x1d45e0, kShader); // complete NEW source settings
    Call(0x19ef90, kTarget); // complete NEW source target setup
    for (uint32_t bucket = 0; bucket < 28; ++bucket) {
        ram::write(kBucket + bucket * 0x38, kRoot + bucket * 0x100);
        ram::write(kBucket + bucket * 0x38 + 0x10, kRoot + bucket * 0x100);
        ram::write<uint8_t>(kBucket + bucket * 0x38 + 0x15, 1);
        ram::write<uint32_t>(kRoot + bucket * 0x100, 0x20000000);
    }
    ram::write(0x323310, kPacket);
    Call(0x1a06a8, 0x400000); // actual software instance pool initialization
    std::memcpy(ram::bytes.data() + kRigid + 0x100, ram::bytes.data() + kRigid, 32);
    const Matrix screen = title::MathLibrary::EvaluateMatrixProduct(clip,
        ram::read<Matrix>(kTarget + 0x30)).output;
    ram::write(kView + 0xc0, screen);
    for (unsigned i = 0; i < 24; ++i) {
        Matrix local = title::MathLibrary::EvaluateIdentity();
        local[3][0] = float(i % 5) * 0.137f;
        local[3][1] = float(i % 3) * -0.271f;
        local[3][2] = float(i % 4) * 0.043f;
        ram::write(0x100000 + i * 0x100, local);
        ram::write(0x100040 + i * 0x100, title::MathLibrary::EvaluateMatrixProduct(local, screen).output);
        ram::write(0x100080 + i * 0x100, title::MathLibrary::EvaluateMatrixProduct(local, clip).output);
    }
    ram::write(0x90000, title::MathLibrary::EvaluateIdentity());
    ram::write(0x90100, title::MathLibrary::EvaluateIdentity());
    ram::write(0x90200, std::array<float,4>{0.5f,0.5f,0.5f,1});

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
};
struct Result {
    std::vector<Draw> draws;
    std::array<uint8_t,3> flags;
    uint32_t listed = 0;
};
Result Run(bool selected, unsigned variant) {
    Result result;
    Assets assets;
    REQUIRE(assets.ModelLoaded(Reader(), kModel) == Assets::Result::Ok);
    auto resource = std::make_shared<Graphics::Resource>();
    const auto wp = ram::read<uint16_t>(kTexture + 16), hp = ram::read<uint16_t>(kTexture + 18);
    resource->description = {1u << wp, 1u << hp, ps2r::render::SurfaceFormat::Rgba8, false, 1};
    resource->lifetime = std::make_shared<int>(1);
    title::MeshMaterial::Texture texture;
    texture.lease = resource;
    texture.width = 1u << wp; texture.height = 1u << hp;
    texture.buffer_width = ram::read<uint32_t>(kTexture + 12 + 0x2c) * 64;
    texture.levels = ram::read<uint8_t>(kTexture + 20);
    texture.source_format = ram::read<uint8_t>(kTexture + 21);
    texture.alpha = ram::read<uint8_t>(kTexture + 23);
    texture.function = ram::read<uint8_t>(kTexture + 25);
    Pass::Hooks hooks;
    hooks.enabled = selected;
    hooks.read = [reader = Reader()](uint32_t address, void* bytes, size_t size) {
        // Selected draws may read source objects/matrices, never the authored
        // packet payload, generated material registers, or producer arena.
        if ((address >= 0x23000 && address < 0x40000) ||
            (address >= 0x80000 && address < 0x90000) ||
            (address >= 0x600000 && address < 0x690000)) return false;
        return reader(address, bytes, size);
    };
    hooks.write = [](const std::vector<Pass::Write>& writes) {
        for (const auto& write : writes)
            if (write.address > ram::bytes.size() || write.bytes.size() > ram::bytes.size() - write.address) return false;
        for (const auto& write : writes)
            std::memcpy(ram::bytes.data() + write.address, write.bytes.data(), write.bytes.size());
        return true;
    };
    hooks.model = [&](uint32_t address) { return assets.FindModel(address); };
    hooks.texture = [&](uint32_t address, title::MeshMaterial::Texture& out) {
        if (address != kTexture) return false;
        out = texture; return true;
    };
    hooks.environment = [](title::MaterialState::Environment& out) { out = Environment(); return true; };
    hooks.owns_material = [](uint32_t address) { return address == kMaterial; };
    hooks.reserve = [&](const std::vector<Pass::Request>& requests, std::vector<Pass::Record>& records) {
        for (const auto& request : requests) {
            REQUIRE(request.material == kMaterial && request.shader.texture.lease == resource);
            records.push_back([&](const std::vector<Draw>& draws) {
                result.draws.insert(result.draws.end(), draws.begin(), draws.end()); return true;
            });
        }
        return true;
    };
    std::vector<uint8_t> packet_before;
    if (selected) packet_before.assign(ram::bytes.begin()+0x23000, ram::bytes.begin()+0x40000);
    Pass pass(std::move(hooks));
    REQUIRE(pass.Install([](uint32_t) -> Pass::Body {
        return [](CpuContext& context, uint32_t at) {
            if (at == title::MeshIntent::kRigid || at == title::MeshIntent::kPlaced || at == title::MeshIntent::kFlushMaterial)
                ++model_bodies;
            rigid_content_oracle::Original(at, context);
        };
    }, [](uint32_t address, Pass::Body body) { rigid_content_oracle::overrides[address] = std::move(body); return true; }));
    if (!selected) {
        Call(0x1da060); // unchanged resident/program inputs, model arm only
        Call(0x1bbec0);
    }
    const unsigned instance_count = 3 + variant * 3;
    for (unsigned i = 0; i < instance_count; ++i) {
        const uint32_t object = kRigid + (i % 2) * 0x100;
        const uint32_t mode = i % 3 ? 1 : 2;
        if (variant % 2) {
            ram::write(kScreen + 0x80, ram::read<Matrix>(0x100040 + i * 0x100));
            ram::write(kScreen + 0x40, ram::read<Matrix>(0x100080 + i * 0x100));
            ram::write(kScreen + 0xc0, ram::read<std::array<float,4>>(kView + 0x140));
            ram::write(kScreen + 0xf8, mode);
            try { Call(title::MeshIntent::kPlaced, object, kScreen, 0x100000 + i * 0x100); }
            catch (const std::exception& error) { std::fprintf(stderr, "case%zu selected%d placed%u: %s\n", cases, selected, i, error.what()); throw; }
        } else {
            auto context = Context();
            context.cpuRegs.GPR.r[4].UL[0] = object;
            context.cpuRegs.GPR.r[5].UL[0] = 0x100000 + i * 0x100;
            context.cpuRegs.GPR.r[6].UL[0] = 0x90000;
            context.cpuRegs.GPR.r[7].UL[0] = 0x90100;
            context.cpuRegs.GPR.r[8].UL[0] = 0x90200;
            context.cpuRegs.GPR.r[9].UL[0] = mode;
            try { rigid_content_oracle::Run(title::MeshIntent::kRigid, context); }
            catch (const std::exception& error) { std::fprintf(stderr, "case%zu selected%d rigid%u: %s\n", cases, selected, i, error.what()); throw; }
        }
    }
    Call(title::MeshIntent::kClose);
    Call(0x1c0490); // unchanged FlushMaterials, selected complete FlushMaterial hook
    result.flags = ram::read<std::array<uint8_t,3>>(kMaterial + 0x64);
    result.listed = ram::read<uint32_t>(Pass::kMaterialCount);
    if (selected) {
        const auto stats = pass.Stats();
        REQUIRE(stats.rigid.replaced + stats.placed.replaced == instance_count && stats.flush.replaced == 1);
        REQUIRE(!stats.rigid.model_calls && !stats.placed.model_calls && !stats.flush.model_calls && !stats.faults);
        REQUIRE(stats.instances == instance_count);
        REQUIRE(std::equal(packet_before.begin(), packet_before.end(), ram::bytes.begin()+0x23000));
    } else {
        for (uint32_t bucket = 0; bucket < 28; ++bucket) {
            const uint32_t tail = ram::read<uint32_t>(kBucket + bucket * 0x38 + 0x10);
            ram::write<uint32_t>(tail, bucket == 27 ? 0x70000000 : 0x20000000);
            ram::write<uint32_t>(tail + 4, kRoot + (bucket + 1) * 0x100);
        }
        auto renderer = std::make_unique<ModelRenderer>();
        renderer->Render();
        result.draws = std::move(renderer->sink.draws);
    }
    Call(Pass::kForget);
    REQUIRE(ram::read<uint32_t>(Pass::kMaterialCount) == 0);
    rigid_content_oracle::overrides.clear();
    return result;
}
void Normalize(Draw& draw) {
    // Model job numbering/upload epochs have no renderer meaning in the
    // selected path. Preserve actual draw order with the compared row index.
    draw.seq = draw.vram_epoch = 0;
    // Physical texture layout is replaced by an owned decoded image. The
    // authored mip count and signed sampling controls remain compared below.
    draw.tex.tbp = draw.tex.cbp = 0;
    for (size_t i = 0; i != 6; ++i) draw.tex.mip_tbp[i] = draw.tex.mip_tbw[i] = 0;
    if (!draw.alpha_blend) draw.raster.alpha = {};
}
void Compare(Result model, Result selected) {
    REQUIRE(model.flags == selected.flags && model.listed == selected.listed);
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
    REQUIRE(assets.ModelLoaded(Reader(), kModel) == Assets::Result::Ok);
    const auto old = assets.FindModel(kModel);
    REQUIRE(old && !old->parts.empty());
    const auto vertex = old->parts[0].strips[0].vertices[0].position;
    assets.ForgetModel(kModel);
    REQUIRE(!assets.FindModel(kModel));
    REQUIRE(old->parts[0].strips[0].vertices[0].position == vertex);
    REQUIRE(assets.ModelLoaded(Reader(), kModel) == Assets::Result::Ok);
    REQUIRE(assets.FindModel(kModel) != old);
    const uint32_t size = ram::read<uint32_t>(kList + 0x400);
    ram::write<uint32_t>(kList + 0x400, 0xfffffff0);
    REQUIRE(assets.ModelLoaded(Reader(), kModel) != Assets::Result::Ok);
    REQUIRE(!assets.FindModel(kModel) && old->parts[0].strips[0].vertices[0].position == vertex);
    ram::write(kList + 0x400, size);
}
}  // namespace

int main(int argc,char** argv) {
    if(argc<3 || !std::filesystem::is_directory(argv[2])) return 77;
    const auto elf=File(argv[1]); if(elf.empty()) return 77;
    if(argc>3) comparison.open(argv[3]);
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
    std::ifstream parts(std::filesystem::path(argv[2]) / "parts.tsv");
    std::string model_file;
    uint32_t part = 0;
    REQUIRE(bool(parts >> model_file >> part));
    const auto authored = File(std::filesystem::path(argv[2]) / model_file);
    size_t offset = 4;
    for (uint32_t i = 0; i < part; ++i) {
        offset += 8 + Field<uint32_t>(authored, offset + 4);
        offset += 4 + Field<uint32_t>(authored, offset);
    }
    const size_t size = Field<uint32_t>(authored, offset + 4);
    std::vector<uint8_t> model(4 + 8 + size + 4);
    const uint32_t one = 1;
    std::memcpy(model.data(), &one, 4);
    std::memcpy(model.data() + 4, authored.data() + offset, 8 + size);
    for(size_t i=0;i<materials.size();++i) {
        const auto bytes = File(materials[i]);
        const size_t start = 16 + Field<uint32_t>(bytes,12);
        if (bytes.size() != start + 98 || Field<uint32_t>(bytes,start)!=1 ||
            Field<uint32_t>(bytes,start+4)!=1 || Field<uint64_t>(bytes,0)!=8 ||
            !bytes[start+8+11] || !bytes[start+8+12]) continue;
        if (i % 37) continue;
        const auto material=ReadMaterial(materials[i]);
        const auto texture=File(textures.at(material.texture));
        const unsigned variant = unsigned(cases % 8);
        Fixture(elf,model,material,texture,variant);
        if (!cases) AssetLifetimes();
        auto input=std::make_unique<decltype(ram::bytes)>();
        std::memcpy(input->data(),ram::bytes.data(),ram::bytes.size());
        const auto expected=Run(false, variant);
        std::memcpy(ram::bytes.data(),input->data(),ram::bytes.size());
        constexpr int modes[]{FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD};
        std::fesetround(modes[cases%4]);
        const auto selected=Run(true, variant);
        REQUIRE(std::fegetround()==modes[cases%4]);
        Compare(expected,selected);
    }
    REQUIRE(cases);
    std::fesetround(saved);
    std::printf("MeshPass NEW generated self-oracle: %zu cases, %zu vertices, %zu model boundary calls; zero active DrawIR/vertex differences\n",
                cases,vertices_compared,model_bodies);
}
