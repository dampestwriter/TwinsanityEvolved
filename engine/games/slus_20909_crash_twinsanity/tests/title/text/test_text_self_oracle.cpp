#include <algorithm>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "games/slus_20909_crash_twinsanity/tests/recomp/oracle_artifacts.h"
#include "games/slus_20909_crash_twinsanity/tests/title/text/text_oracle.h"

namespace {
using namespace title;
size_t cases = 0, vertices = 0, draws = 0;
[[noreturn]] void Fail(const char* what, int line) {
    std::fprintf(stderr, "Text SELF case %zu line %d: %s\n", cases, line, what); std::exit(1);
}
#define CHECK(value) do { if (!(value)) Fail(#value, __LINE__); } while (false)
// Artifact export (--export-dir DIR --source-manifest FILE): per case one input
// record for both arms (fixture index, page count, environment, sparse
// non-zero 4 KiB pages of the fixture RAM), the FE_TOWARDZERO arm's draws and
// vertices per page in material order (tex.tbp/cbp zeroed on both arms: the
// host owns an image lease, the model a VRAM slot; Compare() checks the whole
// DrawRec otherwise), and the TextService census of each arm.
retirement::OracleArtifacts artifacts;
TextService::Counters native_census;
std::string Row(const TextService::Counters& c) {
    return "replaced=" + std::to_string(c.replaced) + ",model_calls=" + std::to_string(c.model_calls) +
           ",failed=" + std::to_string(c.failed);
}
template <class T> void Word(retirement::OracleArtifacts::Bytes& out, T value) {
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}
void ExportInput(const TextFixture& f, size_t index, const MaterialState::Environment& e) {
    if (!artifacts.Enabled()) return;
    const auto& ram = f.textures.ram;
    for (bool native : {false, true}) {
        auto& out = artifacts.Input(native);
        Word(out, uint32_t(0x54585453)); // record marker
        Word(out, uint32_t(index)); Word(out, f.textures.Word(f.font + 0x18));
        Word(out, uint8_t(e.colour_clamp)); Word(out, uint8_t(e.per_pixel_alpha)); Word(out, e.scan_mask);
        Word(out, e.dither); Word(out, e.dither_matrix); Word(out, e.fog_r); Word(out, e.fog_g); Word(out, e.fog_b);
        constexpr size_t page = 4096;
        const auto used = [&](size_t at) {
            return std::any_of(ram.begin() + at, ram.begin() + at + page, [](uint8_t b) { return b != 0; });
        };
        uint32_t pages = 0;
        for (size_t at = 0; at < ram.size(); at += page) if (used(at)) ++pages;
        Word(out, pages);
        for (size_t at = 0; at < ram.size(); at += page) {
            if (!used(at)) continue;
            Word(out, uint32_t(at));
            out.insert(out.end(), ram.begin() + at, ram.begin() + at + page);
        }
    }
}
template <class Pages> void ExportPages(bool native, const Pages& pages) {
    if (!artifacts.Enabled()) return;
    auto& out = artifacts.Output(native, "draw");
    Word(out, uint32_t(pages.size()));
    for (const auto& page : pages) {
        Word(out, page.material); Word(out, uint32_t(page.draws.size()));
        for (auto draw : page.draws) {
            draw.tex.tbp = draw.tex.cbp = 0;
            artifacts.RecordDraw(native, draw, true, true);
        }
    }
}
void Compare(const ps2r::gs::DrawIr& model, const ps2r::gs::DrawIr& actual) {
    CHECK(model.verts.size() == actual.verts.size());
    if (std::memcmp(model.verts.data(), actual.verts.data(), model.verts.size() * sizeof(ps2r::gs::VertexRec))) {
        for (size_t n = 0; n < model.verts.size(); ++n) {
            const auto& a = model.verts[n]; const auto& b = actual.verts[n];
            if (std::memcmp(&a, &b, sizeof(a))) {
                std::fprintf(stderr, "vertex %zu: model xy %a %a uv %u %u rgba %u %u %u %u; actual xy %a %a uv %u %u rgba %u %u %u %u\n",
                    n, a.x,a.y,a.uv_u,a.uv_v,a.r,a.g,a.b,a.a,b.x,b.y,b.uv_u,b.uv_v,b.r,b.g,b.b,b.a);
                break;
            }
        }
        CHECK(false);
    }
    const auto a = model.ToDrawRec(), b = actual.ToDrawRec();
    if (std::memcmp(&a, &b, sizeof(a))) {
        const auto* aa = reinterpret_cast<const uint8_t*>(&a);
        const auto* bb = reinterpret_cast<const uint8_t*>(&b);
        for (size_t n = 0; n < sizeof(a); ++n)
            if (aa[n] != bb[n]) std::fprintf(stderr, "DrawRec byte %zu: model %x actual %x\n", n, aa[n], bb[n]);
        CHECK(false);
    }
    CHECK(model.raster.dthe == actual.raster.dthe && model.raster.dimx == actual.raster.dimx);
    CHECK(model.tex.k == actual.tex.k && model.tex.l == actual.tex.l && model.tex.mxl == actual.tex.mxl &&
          model.tex.lcm == actual.tex.lcm);
    vertices += actual.verts.size(); ++draws;
}
void Case(TextFixture& f, size_t index) {
    ++cases;
    auto& t = f.textures;
    const uint32_t width = index & 1 ? 512 : 384, height = index & 2 ? 448 : 224;
    t.Put(MaterialState::kDisplayWidth, width); t.Put(MaterialState::kDisplayHeight, height);
    t.Put(TextIntent::kRendererExtent, width + index % 13, 2);
    t.Put(TextIntent::kRendererExtent + 2, height + index % 7, 2);
    constexpr uint32_t target = 0x160000;
    t.Put(MaterialState::kRenderTarget, target);
    t.Put(target + 4, index % 17); t.Put(target + 8, index % 19);
    t.Put(target + 0xc, width); t.Put(target + 0x10, height);
    t.Put(target + 0x18, width); t.Put(target + 0x1c, height);
    t.Put(target + 0x68, 0x1e2000);
    for (uint32_t page = 0; page < t.Word(f.font + 0x18); ++page) {
        const uint32_t shader = t.shader + page * 0x100;
        uint64_t settings = uint64_t(index & 1) | uint64_t(index % 6) << 1 |
            uint64_t(index % 0x10000) << 5 | uint64_t(index % 4) << 21 |
            uint64_t((index >> 2) & 1) << 26 | uint64_t{1} << 27 |
            uint64_t((index >> 1) & 1) << 56 | uint64_t((index >> 2) & 1) << 59;
        if (index & 4)
            settings |= uint64_t{1} << 38 | uint64_t(index % 3) << 39 |
                        uint64_t((index + 1) % 3) << 41 | uint64_t((index + 2) % 3) << 43 |
                        uint64_t(index % 3) << 45 | uint64_t(index * 7 % 256) << 47;
        t.Put(shader, settings, 8);
        t.Put(shader + 0x18, uint16_t(index & 1 ? -69 : -120), 2);
        t.Put(shader + 0x1a, index % 4, 2);
        t.Put(f.material + page * 0x100 + 0x60, index & 1 ? 2 : 1);
    }
    t.Put(f.queue + 8, 0);
    for (uint32_t item = 0; item < 2; ++item) {
        TextLayout::Item input;
        input.x = float(int(index % 7) - 3) / 3; input.y = float(int(index % 11) - 5) / 4;
        input.scale_x = index & 4 ? -.71f : 1.13f; input.scale_y = index & 8 ? -1.37f : .83f;
        input.colour = 0x83905a24u ^ uint32_t(index * 0x151713) ^ (item * 0x27182818u);
        input.alignment = uint32_t(index * 5) & 0x7f;
        switch (index % 8) {
            case 0: input.text = ""; break;
            case 1: input.text = "~a~b~~~c~"; break;
            case 2: input.text.assign(608, 'a'); break;
            case 3:
                for (size_t n = 0; n < 60; ++n) input.text += "abcDEF!~";
                break;
            case 4: input.text = "Crash\xff\x81~Twinsanity"; break;
            default: input.text = "Crash~Twinsanity! 123~~aBc \xc2\x80"; break;
        }
        f.SetItem(item, input);
    }
    MaterialState::Environment environment;
    environment.colour_clamp = index & 1; environment.per_pixel_alpha = index & 2;
    environment.dither = (index >> 2) & 1; environment.dither_matrix = 0x65437120;
    environment.scan_mask = index % 4; environment.fog_r = uint8_t(index);
    ExportInput(f, index, environment);
    const auto model = TextOracle::Run(f, environment);
    ExportPages(false, model);
    const auto before_calls = TextOracle::Calls();
    const auto ram = t.ram;
    for (int rounding : {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD}) {
        std::fesetround(rounding);
        TextService::Hooks hooks;
        hooks.enabled = true; hooks.read = f.Reader();
        hooks.environment = [&](MaterialState::Environment& out) { out = environment; return true; };
        hooks.selected = [&](uint32_t font) { return font == f.font; };
        TextIntent::Output actual;
        hooks.record = [&](const TextIntent::Output& out) { actual = out; return true; };
        TextService service(t.bindings, hooks);
        std::map<uint32_t, TextService::Body> entries;
        CHECK(service.Install([](uint32_t) -> TextService::Body {
            return [](CpuContext&, uint32_t) { CHECK(false); };
        }, [&](uint32_t entry, TextService::Body body) { entries[entry] = std::move(body); return true; }));
        CpuContext c{}; c.cpuRegs.GPR.r[5].UL[0] = f.queue;
        entries.at(TextIntent::kDrawFontTexts)(c, TextIntent::kDrawFontTexts);
        CHECK(std::fegetround() == rounding && t.ram == ram && TextOracle::Calls() == before_calls);
        CHECK(service.Counts().replaced == 1 && service.Counts().model_calls == 0 && service.Counts().failed == 0);
        CHECK(actual.pages.size() == model.size() && actual.source_items == 2);
        for (size_t page = 0; page < model.size(); ++page) {
            CHECK(actual.pages[page].material == model[page].material);
            CHECK(actual.pages[page].draws.size() == model[page].draws.size());
            for (size_t draw = 0; draw < model[page].draws.size(); ++draw)
                Compare(model[page].draws[draw], actual.pages[page].draws[draw]);
        }
        if (rounding == FE_TOWARDZERO) { // the NEW caller contract; the exported arm
            ExportPages(true, actual.pages);
            const auto c = service.Counts();
            native_census.calls += c.calls; native_census.replaced += c.replaced;
            native_census.model_calls += c.model_calls; native_census.failed += c.failed;
        }
    }
}
void Run(uint32_t pages, const TextLayout::Font* original = nullptr, bool visible_padding = false) {
    TextFixture f(pages);
    if (original) f.SetFont(*original);
    if (visible_padding) {
        auto font = f.data;
        uint32_t bits;
        std::memcpy(&bits, &font.glyphs[0].u, 4); bits = (bits & ~3u) | 1;
        std::memcpy(&font.glyphs[0].u, &bits, 4);
        f.SetFont(font);
    }
    for (size_t index = 0; index < 32; ++index) Case(f, index);
}
}
int main(int argc, char** argv) {
    int export_first = argc;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--export-dir") { export_first = i; break; }
    CHECK(artifacts.Parse(argc, argv, export_first));
    Run(1); Run(3); Run(3, nullptr, true);
    size_t originals = 0;
    if (export_first == 2) {
        const std::filesystem::path manifest(argv[1]);
        std::ifstream list(manifest);
        CHECK(bool(list));
        uint32_t pages = 0, count = 0;
        TextLayout::Font font; font.width = .02f; font.height = .03f;
        std::string file;
        while (list >> pages >> font.first_character >> count >> file) {
            CHECK(count && count <= 256 && pages && pages <= 3);
            font.glyphs.resize(count);
            std::ifstream glyphs(manifest.parent_path() / file, std::ios::binary);
            CHECK(bool(glyphs.read(reinterpret_cast<char*>(font.glyphs.data()), count * 16)));
            CHECK(glyphs.peek() == std::char_traits<char>::eof());
            Run(pages, &font); ++originals;
        }
        CHECK(list.eof() && originals != 0);
    }
    std::fesetround(FE_TONEAREST);
    std::printf("Text SELF: %zu cases, %zu original font glyph arrays, %zu draws/%zu vertices, four rounding modes, zero selected model calls\n",
                cases, originals, draws, vertices);
    if (artifacts.Enabled()) {
        artifacts.Counter(false, "graphics_text", Row(TextOracle::ModelCensus()));
        artifacts.Counter(true, "graphics_text", Row(native_census));
        artifacts.Setting("hle.graphics_text", "off", "on");
        artifacts.Setting("rounding", "FE_TOWARDZERO", "FE_TOWARDZERO");
        artifacts.Note("scope", "whole DrawFontTexts producer (TR text.cpp Begin/Draw/End batching replaced by the "
            "owned typesetter): per page material and canonical DrawRec plus out-of-record dither/mip fields and "
            "vertices; component fixture fonts/pages plus any supplied original glyph arrays; no rasterizer claim");
        artifacts.Note("canonicalization", "tex.tbp/cbp zeroed on both arms (host image lease vs model VRAM slot); "
            "inactive ALPHA and inactive texture zeroed; pages in the model's material order");
        artifacts.Note("counter_provenance", "graphics_text: TextService census published by hle/graphics/"
            "graphics_report.cpp; the model arm executes the generated DrawFontTexts through a model-mode TextService "
            "(the catalog's ++counts_.model_calls; original(c, entry) increment), the native arm through the selected "
            "service with zero model calls");
        artifacts.Finish("TextSelfOracle", "text-artifacts-v1", argc, argv);
        std::printf("TextSelfOracle artifacts: model %s / native %s\n",
                    Row(TextOracle::ModelCensus()).c_str(), Row(native_census).c_str());
    }
}
