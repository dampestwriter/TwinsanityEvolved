#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

#include <algorithm>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "engine/gs/state/draw_flush.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_service.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.h"
#include "games/slus_20909_crash_twinsanity/tests/title/textures/texture_binding_fixture.h"
#include "tests/lib/capture_sink.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
std::map<uint32_t, size_t> calls;
uint32_t end = 0;
void ObserveGenerated(uint32_t address) { ++calls[address]; }
void ExternalCall(uint32_t address, CpuContext& context) {
    if (address == 0x1a6068) { context.cpuRegs.GPR.r[2].UL[0] = 0x170000; return; }
    if (address == 0x1a6190) { end = context.cpuRegs.GPR.r[4].UL[0]; return; }
    if (address == 0x1815c0 && context.cpuRegs.GPR.r[4].UL[0] <= 0x100) {
        context.cpuRegs.GPR.r[2].UL[0] = 0x180000; return;
    }
    std::fprintf(stderr, "unexpected generated callee %x\n", address);
    std::abort();
}
}
namespace {
using namespace title;
using F = TextureBindingFixture;
namespace ram = strip_oracle::memory;
size_t cases = 0, vertices = 0, empty_cases = 0;
size_t continuations = 0;
std::ofstream evidence;
[[noreturn]] void Fail(const char* expression, int line) {
    std::fprintf(stderr, "case %zu line %d: %s\n", cases, line, expression); std::exit(1);
}
#define CHECK(value) do { if (!(value)) Fail(#value, __LINE__); } while (false)
// Artifact export (--export-dir DIR --source-manifest FILE): per case one input
// record for both arms, the FE_TOWARDZERO arm's draws/vertices (tex.tbp/cbp
// zeroed on both arms: the host owns an image lease, the model a VRAM slot),
// and the SpriteService census of each arm. The model arm runs the SAME
// generated bodies through a model-mode SpriteService so its model_calls are
// the catalog's Graphics.sprites.model_calls increment, not a direct call.
retirement::OracleArtifacts artifacts;
SpriteService::Counters model_census, native_census;
void Add(SpriteService::Counters& total, const SpriteService::Counters& c) {
    total.calls += c.calls; total.replaced += c.replaced; total.model_calls += c.model_calls;
    total.failed += c.failed; total.empty += c.empty;
}
std::string Row(const SpriteService::Counters& c) {
    return "replaced=" + std::to_string(c.replaced) + ",model_calls=" + std::to_string(c.model_calls) +
           ",failed=" + std::to_string(c.failed) + ",empty=" + std::to_string(c.empty);
}
template <class T> void Word(retirement::OracleArtifacts::Bytes& out, T value) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}
void ExportInput(const F& f, unsigned variant, size_t index, bool turned, bool collapsed, uint32_t colour,
                 const MaterialState::Environment& e) {
    if (!artifacts.Enabled()) return;
    for (bool native : {false, true}) {
        auto& out = artifacts.Input(native);
        Word(out, uint32_t(0x54505253)); // record marker
        Word(out, uint32_t(variant)); Word(out, uint32_t(index)); Word(out, uint8_t(turned)); Word(out, uint8_t(collapsed));
        Word(out, colour);
        Word(out, uint8_t(e.colour_clamp)); Word(out, uint8_t(e.per_pixel_alpha)); Word(out, e.scan_mask);
        Word(out, e.dither); Word(out, e.dither_matrix); Word(out, e.fog_r); Word(out, e.fog_g); Word(out, e.fog_b);
        // Sparse non-zero 4 KiB pages of the fixture RAM the generated body reads.
        constexpr size_t page = 4096;
        uint32_t pages = 0;
        for (size_t at = 0; at < f.ram.size(); at += page)
            if (std::any_of(f.ram.begin() + at, f.ram.begin() + at + page, [](uint8_t b) { return b != 0; })) ++pages;
        Word(out, pages);
        for (size_t at = 0; at < f.ram.size(); at += page) {
            if (!std::any_of(f.ram.begin() + at, f.ram.begin() + at + page, [](uint8_t b) { return b != 0; })) continue;
            Word(out, uint32_t(at));
            out.insert(out.end(), f.ram.begin() + at, f.ram.begin() + at + page);
        }
    }
}
void ExportDraws(bool native, const std::vector<ps2r::gs::DrawIr>& draws) {
    if (!artifacts.Enabled()) return;
    Word(artifacts.Output(native, "draw"), uint32_t(draws.size()));
    for (auto draw : draws) {
        draw.tex.tbp = draw.tex.cbp = 0;
        artifacts.RecordDraw(native, draw, true, true);
    }
}
// Runs a generated Sprite entry the way the title does: through SpriteService
// in model mode, so the census increment is the one the catalog binds.
TextureBindings* model_bindings = nullptr;
void RunModel(uint32_t function, CpuContext& c) {
    CHECK(model_bindings);
    SpriteService::Hooks hooks;
    hooks.enabled = false;
    SpriteService service(*model_bindings, hooks);
    std::map<uint32_t, SpriteService::Body> installed;
    CHECK(service.Install([](uint32_t entry) {
        return [entry](CpuContext& ctx, uint32_t) { strip_oracle::RunGenerated(entry, ctx); };
    }, [&](uint32_t entry, SpriteService::Body body) { installed[entry] = std::move(body); return true; }));
    installed.at(function)(c, function);
    CHECK(service.Counts().model_calls == 1 && service.Counts().replaced == 0);
    Add(model_census, service.Counts());
}
uint32_t random_bits = 0x38272985;
uint32_t Next() { random_bits ^= random_bits << 13; random_bits ^= random_bits >> 17; random_bits ^= random_bits << 5; return random_bits; }
constexpr uint32_t kShape = 0x140000, kMaterial = 0x150000, kTarget = 0x160000, kMatrix = 0x1a0000;
CpuContext Context(uint32_t colour, unsigned variant) {
    CpuContext c{};
    c.cpuRegs.GPR.r[28].UL[0] = 0x3114f0;
    c.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    c.cpuRegs.GPR.r[4].UL[0] = kShape;
    c.cpuRegs.GPR.r[5].UL[0] = variant == 1 ? colour : kMatrix;
    c.cpuRegs.GPR.r[6].UL[0] = colour;
    c.vuRegs.VF[0].w = 1;
    return c;
}
void Float(F& f, uint32_t at, float value) {
    uint32_t word; std::memcpy(&word, &value, 4); f.Put(at, word);
}
MaterialState::Environment Input(F& f, size_t index, bool turned) {
    constexpr unsigned types[]{2, 14, 19, 13};
    const unsigned type = types[index % 4];
    // Actual PSM UI materials use type 0D with STQ, unlike font text's fixed
    // UV use of that type. Its authored K=-1 and linear/clamp policy matter.
    f.Shader(type, type == 13 ? -1 : index & 1 ? -120 : -69,
             type == 13 ? 0 : index % 4, index & 1, true);
    auto settings = uint64_t(f.Word(F::shader)) | uint64_t(f.Word(F::shader + 4)) << 32;
    settings |= uint64_t(index & 1) | (uint64_t(index % 4) << 21) |
                uint64_t((index >> 1) & 1) << 59 | uint64_t((index >> 2) & 1) << 56 |
                uint64_t(index & 1) << 26;
    // Active tests/blend vary; all producer state must override its predecessor.
    settings |= uint64_t(Next() & 0xffff) << 5;
    settings |= uint64_t(index % 6) << 1;
    if (index & 2) {
        settings |= uint64_t{1} << 38 | uint64_t(Next() % 3) << 39 |
                    uint64_t(Next() % 3) << 41 | uint64_t(Next() % 3) << 43 |
                    uint64_t(Next() % 3) << 45 | uint64_t(Next() & 255) << 47;
    }
    f.Put(F::shader, settings, 8);
    f.Put(kShape, kMaterial); f.Put(kShape + 8, turned, 1); f.Put(kShape + 0xc, 0x2f6b50);
    for (uint32_t lane = 0; lane < 4; ++lane)
        Float(f, kShape + 0x10 + lane * 4, float(int(Next() % 2049) - 1024) / 1024);
    f.Put(kMaterial, F::shader); f.Put(kMaterial + 0x10, 1); f.Put(kMaterial + 0x20, 24);
    f.Put(MaterialState::kRenderTarget, kTarget);
    const uint32_t w = index & 1 ? 512 : 384, h = index & 2 ? 448 : 224;
    f.Put(MaterialState::kDisplayWidth, w); f.Put(MaterialState::kDisplayHeight, h);
    f.Put(MaterialState::kDisplayPixelBytes, 2); f.Put(MaterialState::kDepthBufferPage, 168);
    f.Put(PrimitiveMaterial::kScreenWidth, w + index % 7);
    f.Put(PrimitiveMaterial::kScreenHeight, h + index % 11);
    f.Put(kTarget + 4, index % 17); f.Put(kTarget + 8, index % 19);
    f.Put(kTarget + 0xc, w); f.Put(kTarget + 0x10, h);
    f.Put(kTarget + 0x18, w + index % 13); f.Put(kTarget + 0x1c, h + index % 23);
    f.Put(kTarget + 0x68, 0x190000);
    for (uint32_t i = 0; i < 16; ++i)
        Float(f, kMatrix + i * 4, float(int(Next() % 65537) - 32768) / 8192);
    constexpr uint64_t presets[]{0x44, 0x48, 0x42, 0x49, 0x46, 0x89};
    for (uint32_t i = 0; i < 6; ++i) f.Put(0x3d6b20 + i * 8, presets[i], 8);
    MaterialState::Environment env;
    env.colour_clamp = index & 1; env.per_pixel_alpha = index & 2; env.dither = (index >> 2) & 1;
    env.scan_mask = index % 4; env.dither_matrix = uint64_t(Next()) | uint64_t(Next()) << 32;
    env.fog_r = uint8_t(Next()); env.fog_g = uint8_t(Next()); env.fog_b = uint8_t(Next());
    return env;
}
void Pairs(ps2r::gs::GsRegisters& regs, ps2r::gs::VertexKick& kick, uint32_t begin, uint32_t end) {
    CHECK(end >= begin && !((end - begin) & 15));
    for (uint32_t at = begin; at < end; at += 16) {
        const auto value = ram::read<uint64_t>(at);
        const auto address = ram::read<uint64_t>(at + 8);
        if (address == 5 || address == 13) kick.WriteXyz(address == 5, value);
        else {
            if (!regs.Write(uint8_t(address), value)) {
                std::fprintf(stderr, "unexpected oracle pair at %x: value=%llx address=%llx\n",
                    at, static_cast<unsigned long long>(value), static_cast<unsigned long long>(address));
                CHECK(false);
            }
        }
    }
}
void Compare(const ps2r::gs::DrawIr& model, const ps2r::gs::DrawIr& actual) {
    CHECK(model.verts.size() == actual.verts.size());
    if (std::memcmp(model.verts.data(), actual.verts.data(), model.verts.size() * sizeof(ps2r::gs::VertexRec))) {
        for (size_t i = 0; i < model.verts.size(); ++i) {
            const auto& a = model.verts[i]; const auto& b = actual.verts[i];
            std::fprintf(stderr, "vertex%zu model %a,%a st %a,%a rgba%x/%x/%x/%x; actual %a,%a st %a,%a rgba%x/%x/%x/%x\n",
                i, a.x,a.y,a.s,a.t,a.r,a.g,a.b,a.a,b.x,b.y,b.s,b.t,b.r,b.g,b.b,b.a);
        }
        CHECK(false);
    }
    CHECK(model.snapshot.vertex_hash == actual.snapshot.vertex_hash);
    CHECK(std::memcmp(model.bbox, actual.bbox, sizeof(model.bbox)) == 0);
    CHECK(model.prim_type == actual.prim_type && model.gouraud == actual.gouraud &&
          model.alpha_blend == actual.alpha_blend && model.use_fst == actual.use_fst &&
          model.tex_enabled == actual.tex_enabled && model.ctx == actual.ctx);
#define SAME(field) CHECK(model.field == actual.field)
    SAME(off_x); SAME(off_y); SAME(fog); SAME(scanmsk); SAME(vertex_count);
    SAME(raster.frame.fbp); SAME(raster.frame.fbw); SAME(raster.frame.psm); SAME(raster.frame.fbmsk);
    SAME(raster.zbuf.zbp); SAME(raster.zbuf.psm); SAME(raster.zbuf.zmsk);
    SAME(raster.test.ate); SAME(raster.test.atst); SAME(raster.test.aref); SAME(raster.test.afail);
    SAME(raster.test.date); SAME(raster.test.datm); SAME(raster.test.zte); SAME(raster.test.ztst);
    SAME(raster.alpha.a); SAME(raster.alpha.b); SAME(raster.alpha.c); SAME(raster.alpha.d); SAME(raster.alpha.fix);
    SAME(raster.scissor.x0); SAME(raster.scissor.x1); SAME(raster.scissor.y0); SAME(raster.scissor.y1);
    SAME(raster.fba); SAME(raster.colclamp); SAME(raster.pabe); SAME(raster.dimx); SAME(raster.dthe);
    SAME(raster.fogcol_r); SAME(raster.fogcol_g); SAME(raster.fogcol_b);
    // Storage addresses are replaced by retained image ownership. Compare the
    // complete active sampling contract, without inactive CLUT/VRAM fields.
    SAME(tex.psm); SAME(tex.tw); SAME(tex.th); SAME(tex.tcc); SAME(tex.tfx); SAME(tex.fst);
    SAME(tex.k); SAME(tex.l); SAME(tex.lcm); SAME(tex.mmin); SAME(tex.mmag); SAME(tex.mxl);
    SAME(tex.wms); SAME(tex.wmt); SAME(tex.minu); SAME(tex.maxu); SAME(tex.minv); SAME(tex.maxv);
#undef SAME
    vertices += actual.verts.size();
}
void VisibleMatrix(void* memory) {
    constexpr float matrix[]{1,0,0,0, 0,1,0,0, 0,0,1,0, .25f,.25f,0,1};
    std::memcpy(memory,matrix,sizeof(matrix));
}
void Emit(unsigned variant,int rounding,const char* step,
          const ps2r::gs::DrawIr* model,const ps2r::gs::DrawIr* host) {
    if (!evidence) return;
    using Format = ps2r::gs::GsdlFormat;
    uint64_t a = 0,b = 0;
    if (model && host) {
        const auto m = model->ToDrawRec(),h = host->ToDrawRec();
        a = Format::Fnv1a(&m,sizeof(m)); b = Format::Fnv1a(&h,sizeof(h));
    }
    // Raw storage identity differs because the host owns an image lease.
    // Compare() separately requires every active material field and vertex.
    evidence << cases << '\t' << variant << '\t' << rounding << '\t' << step << '\t'
        << (host ? host->verts.size() : 0) << '\t' << std::hex << a << '\t' << b << '\t'
        << (model ? model->snapshot.vertex_hash : 0) << '\t'
        << (host ? host->snapshot.vertex_hash : 0) << std::dec << "\t0\t0\n";
}
void Case(F& f, unsigned variant, size_t index, bool turned,bool collapsed = false) {
    ++cases;
    const auto environment = Input(f, index, turned);
    if (collapsed) std::memset(f.ram.data()+kMatrix,0,64);
    const uint32_t colour = Next();
    ExportInput(f, variant, index, turned, collapsed, colour, environment);
    constexpr uint32_t entries[]{SpriteIntent::kPlain, SpriteIntent::kColoured, SpriteIntent::kPlaced, SpriteIntent::kPlacedColoured};
    const auto read = [&](uint32_t address, void* out, size_t size) {
        const auto at = address & 0x1fffffff;
        if (at > f.ram.size() || size > f.ram.size() - at) return false;
        std::memcpy(out, f.ram.data() + at, size); return true;
    };
    std::copy(f.ram.begin(), f.ram.end(), ram::bytes.begin());
    std::fesetround(FE_TOWARDZERO);
    auto c = Context(colour, variant);
    c.cpuRegs.GPR.r[4].UL[0] = kTarget; strip_oracle::RunGenerated(0x19ef90, c);
    c = Context(colour, variant); c.cpuRegs.GPR.r[4].UL[0] = F::shader;
    constexpr uint32_t methods[]{0x1cf7f0, 0x1d3f38, 0x1cdd10, 0x1ce778};
    const uint32_t method = methods[index % 4];
    strip_oracle::RunGenerated(method, c);
    ps2r::gs::GsRegisters regs;
    ps2r::gs::VertexKick kick(&regs);
    // SAME explicit frame environment on both paths, including an incompatible
    // prior blend/dither state. Generated target/settings must overwrite it.
    regs.env.colclamp = environment.colour_clamp; regs.env.pabe = environment.per_pixel_alpha;
    regs.env.dthe = environment.dither; regs.env.dimx = environment.dither_matrix;
    regs.env.scanmsk = environment.scan_mask; regs.env.fogcol_r = environment.fog_r;
    regs.env.fogcol_g = environment.fog_g; regs.env.fogcol_b = environment.fog_b;
    regs.ctx[0].alpha = {2, 2, 2, 2, 255};
    // The called packet owns a DMA tag and a GIF tag before its three writes.
    Pairs(regs, kick, 0x190020, 0x190050);
    const uint32_t count = ram::read<uint32_t>(F::shader + 0x5c);
    Pairs(regs, kick, 0x180010, 0x180000 + count * 16);
    // Texture load conversion is independently compared to actual NEW in
    // TextureAssets; here its source dimensions/alpha are the supplied input.
    const uint16_t tw = uint16_t(f.record[8]) | uint16_t(f.record[9]) << 8;
    const uint16_t th = uint16_t(f.record[10]) | uint16_t(f.record[11]) << 8;
    CHECK(tw <= 10 && th <= 10);
    regs.ctx[0].tex0 = {0, uint16_t(1u << tw), f.record[13], uint8_t(tw), uint8_t(th),
                       f.record[15], f.record[16], 0, 0, 0, 0, 0};
    CHECK(regs.Write(0, 4)); kick.OnPrimWritten();
    c = Context(colour, variant); RunModel(entries[variant], c);
    Pairs(regs, kick, 0x170000, strip_oracle::end);
    ps2r::gs::CaptureSink sink;
    ps2r::gs::DrawFlush flush(&regs, &kick); flush.SetSink(&sink);
    const bool nonempty = flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
    CHECK(sink.draws.size() == (nonempty ? 1 : 0));
    if (!nonempty) {
        ++empty_cases;
        SpriteIntent::Output out;
        CHECK(SpriteIntent::Capture(read, f.bindings, entries[variant], kShape,
              variant == 1 ? colour : kMatrix, colour, environment, out) == SpriteIntent::Result::Empty);
    }
    ps2r::gs::CaptureSink following;
    if (!nonempty) {
        CHECK(variant >= 2);
        VisibleMatrix(ram::bytes.data()+kMatrix);
        // Begin2D's PRIM restarts each source strip. The oracle's existing
        // bounded allocation callback does not implement that transport.
        CHECK(regs.Write(0,4)); kick.OnPrimWritten();
        c = Context(colour,variant); RunModel(entries[variant],c);
        Pairs(regs,kick,0x170000,strip_oracle::end);
        flush.SetSink(&following);
        CHECK(flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop) && following.draws.size() == 1);
    }
    {
        std::vector<ps2r::gs::DrawIr> model_draws(sink.draws.begin(), sink.draws.end());
        model_draws.insert(model_draws.end(), following.draws.begin(), following.draws.end());
        ExportDraws(false, model_draws);
    }
    const auto before_calls = strip_oracle::calls;
    const auto before_ram = f.ram;
    for (int rounding : {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD}) {
        std::fesetround(rounding);
        SpriteIntent::Output output;
        SpriteService::Hooks hooks;
        hooks.enabled = true;
        hooks.read = read;
        hooks.selected = [](uint32_t object) { return object == kShape; };
        hooks.environment = [&](MaterialState::Environment& out) { out = environment; return true; };
        unsigned records = 0,empty_records = 0;
        hooks.record = [&](const SpriteIntent::Output& out) {
            if (out.draw.verts.empty()) {
                CHECK(out.bucket == 24 && out.sampling.asset.lease);
                ++empty_records;
                return true;
            }
            ++records; output = out; return true;
        };
        SpriteService service(f.bindings, hooks);
        std::map<uint32_t, SpriteService::Body> bindings;
        CHECK(service.Install([](uint32_t function) {
            return [function](CpuContext& ctx, uint32_t) { strip_oracle::RunGenerated(function, ctx); };
        }, [&](uint32_t function, SpriteService::Body body) { bindings[function] = std::move(body); return true; }));
        auto selected_context = Context(colour, variant);
        bool threw = false;
        try { bindings.at(entries[variant])(selected_context, entries[variant]); }
        catch (const std::runtime_error&) { threw = true; }
        CHECK(!threw && service.Counts().model_calls == 0 && f.ram == before_ram);
        CHECK(std::fegetround() == rounding);
        std::vector<ps2r::gs::DrawIr> native_draws;
        if (nonempty) {
            CHECK(service.Counts().replaced == 1 && service.Counts().failed == 0 && output.bucket == 24);
            CHECK(output.sampling.asset.asset_id == 19 && output.sampling.asset.lease);
            Compare(sink.draws[0], output.draw);
            Emit(variant,rounding,"visible",&sink.draws[0],&output.draw);
            native_draws.push_back(output.draw);
        } else {
            CHECK(service.Counts().replaced == 1 && service.Counts().failed == 0 &&
                  service.Counts().empty == 1 && records == 0 && !output.sampling.asset.lease);
            CHECK(empty_records == 1);
            Emit(variant,rounding,"empty",nullptr,nullptr);
            VisibleMatrix(f.ram.data()+kMatrix);
            selected_context = Context(colour,variant);
            bindings.at(entries[variant])(selected_context,entries[variant]);
            CHECK(service.Counts().calls == 2 && service.Counts().replaced == 2 &&
                  service.Counts().empty == 1 && service.Counts().failed == 0 &&
                  service.Counts().model_calls == 0 && records == 1);
            Compare(following.draws[0],output.draw);
            Emit(variant,rounding,"following",&following.draws[0],&output.draw);
            std::memcpy(f.ram.data()+kMatrix,before_ram.data()+kMatrix,64);
            ++continuations;
            native_draws.push_back(output.draw);
        }
        if (rounding == FE_TOWARDZERO) { // the NEW caller contract; the exported arm
            ExportDraws(true, native_draws);
            Add(native_census, service.Counts());
        }
    }
    CHECK(strip_oracle::calls == before_calls);
}
size_t Originals(F& f, const std::filesystem::path& manifest) {
    std::ifstream list(manifest);
    CHECK(bool(list));
    size_t count = 0;
    std::string file;
    constexpr uint32_t source = 0x210003;
    while (list >> file) {
        std::ifstream input(manifest.parent_path() / file, std::ios::binary | std::ios::ate);
        CHECK(bool(input));
        const auto size = input.tellg();
        CHECK(size > 0 && size <= 0x80000);
        f.record.resize(size_t(size));
        input.seekg(0);
        CHECK(bool(input.read(reinterpret_cast<char*>(f.record.data()), size)));
        std::copy(f.record.begin(), f.record.end(), f.ram.begin() + source);
        f.Put(F::stream + 8, source); f.Put(F::stream + 12, source);
        f.Put(F::stream + 16, f.record.size());
        TextureAssets::View image;
        CHECK(f.bindings.Read(F::object, F::stream, image) == TextureBindings::Result::Ok);
        // All PSM materials declare type 0D/STQ, linear clamp, K=-1/L=0.
        // The actual original header supplies each UI atlas's dimensions,
        // alpha and mip count to both settings paths.
        Case(f, unsigned(count % 4), (count % 8) * 4 + 3, count & 1);
        ++count;
    }
    CHECK(list.eof() && count);
    return count;
}
}
int main(int argc, char** argv) {
    std::filesystem::path manifest;
    int export_first = argc;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--evidence") {
            CHECK(++i < argc);
            evidence.open(argv[i]); CHECK(bool(evidence));
            evidence << "case\tvariant\trounding\tstep\tvertices\tmodel_draw_raw\thost_draw_raw"
                        "\tmodel_vertices\thost_vertices\tactive_differences\tselected_model_calls\n";
        } else if (std::string(argv[i]) == "--export-dir") {
            export_first = i; break; // the pair is parsed whole below
        } else { CHECK(manifest.empty()); manifest = argv[i]; }
    }
    CHECK(artifacts.Parse(argc, argv, export_first));
    auto f = std::make_unique<F>();
    model_bindings = &f->bindings;
    TextureAssets::View image;
    CHECK(f->bindings.Read(F::object, F::stream, image) == TextureBindings::Result::Ok);
    // The source static initializer supplies both unit/turned corners. Do not
    // fabricate its inputs from the replacement's geometry constants.
    std::copy(f->ram.begin(), f->ram.end(), ram::bytes.begin());
    auto initializer = Context(0, 0);
    initializer.cpuRegs.GPR.r[4].UL[0] = 1;
    initializer.cpuRegs.GPR.r[5].UL[0] = 0xffff;
    strip_oracle::RunGenerated(0x1aac90, initializer);
    std::copy(ram::bytes.begin(), ram::bytes.end(), f->ram.begin());
    for (unsigned variant = 0; variant < 4; ++variant)
        for (size_t index = 0; index < 32; ++index)
            for (bool turned : {false, true}) Case(*f, variant, index, turned);
    for (unsigned variant : {2u,3u})
        for (size_t type = 0; type < 4; ++type)
            for (bool turned : {false,true}) Case(*f,variant,type,turned,true);
    image = {};
    const size_t originals = manifest.empty() ? 0 : Originals(*f,manifest);
    CHECK(f->assets.Reset() == TextureAssets::Result::Ok);
    std::fesetround(FE_TONEAREST);
    std::printf("Sprite SELF: %zu cases (%zu complete empty producers), %zu original UI assets, %zu vertices, four rounding modes, zero selected generated/model calls\n",
                cases, empty_cases, originals, vertices);
    std::printf("Sprite empty-to-visible continuations: %zu; zero active material/vertex differences\n",continuations);
    if (artifacts.Enabled()) {
        artifacts.Counter(false, "graphics_sprites", Row(model_census));
        artifacts.Counter(true, "graphics_sprites", Row(native_census));
        artifacts.Setting("hle.graphics_sprites", "off", "on");
        artifacts.Setting("rounding", "FE_TOWARDZERO", "FE_TOWARDZERO");
        artifacts.Note("scope", "UI Sprite producers (plain, coloured, placed, placed+coloured; TR shapes.cpp Sprite/"
            "Shape2D draw entries): canonical active DrawRec plus out-of-record raster/texture fields and vertices; "
            "no rasterizer/presentation claim; component fixture textures plus any supplied original UI asset list");
        artifacts.Note("canonicalization", "tex.tbp/cbp zeroed on both arms (the host owns an image lease, the model a "
            "VRAM slot; Compare() checks every active sampling field); inactive ALPHA and inactive texture zeroed; "
            "an empty producer contributes zero draws and its visible continuation one draw on both arms");
        artifacts.Note("counter_provenance", "graphics_sprites: SpriteService census published by hle/graphics/"
            "graphics_report.cpp; the model arm executes the generated entries through a model-mode SpriteService "
            "(the catalog's ++counts_.model_calls; original(c, entry) increment), the native arm through the "
            "selected service with zero model calls");
        artifacts.Finish("SpriteSelfOracle", "sprite-artifacts-v1", argc, argv);
        std::printf("SpriteSelfOracle artifacts: model %s / native %s\n",
                    Row(model_census).c_str(), Row(native_census).c_str());
    }
}
