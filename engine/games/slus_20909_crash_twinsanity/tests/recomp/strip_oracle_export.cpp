#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_export.h"

#include <cstring>
#include <type_traits>

#include "engine/gs/hle/ir/gsdl_format.h"

namespace strip_oracle {
namespace {
using Bytes = retirement::OracleArtifacts::Bytes;
template <class T> void Word(Bytes& out, T value) {
    static_assert(std::is_integral<T>::value || std::is_floating_point<T>::value);
    uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}
void Append(Bytes& out, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}
std::string Row(const title::GraphicsService::VariantCounters& v) {
    return "calls=" + std::to_string(v.calls) + ",replaced=" + std::to_string(v.replaced) +
           ",model_calls=" + std::to_string(v.model_calls) + ",interior=" + std::to_string(v.interior_fallback);
}
}  // namespace

bool Export::Parse(int argc, char** argv, int first) { return artifacts_.Parse(argc, argv, first); }

void Export::Input(bool native, size_t variant, const Registers& registers,
                   const title::MaterialState::Environment& environment,
                   const uint8_t* ram, size_t size) {
    if (!Enabled()) return;
    ++arms_[native].runs;
    Bytes& out = artifacts_.Input(native);
    Word(out, uint32_t(0x50495254));  // record marker
    Word(out, uint32_t(variant));
    Word(out, registers.a0); Word(out, registers.a1); Word(out, registers.a2);
    Word(out, uint8_t(environment.colour_clamp)); Word(out, uint8_t(environment.per_pixel_alpha));
    Word(out, environment.scan_mask); Word(out, environment.dither);
    Word(out, environment.dither_matrix);
    Word(out, environment.fog_r); Word(out, environment.fog_g); Word(out, environment.fog_b);
    // Sparse image: every non-zero 4 KiB page of the fixture RAM the generated
    // body may read, by page index. The fixture is zero-filled before each case.
    constexpr size_t kPage = 4096;
    std::vector<size_t> pages;
    for (size_t page = 0; page < size / kPage; ++page) {
        const uint8_t* start = ram + page * kPage;
        bool zero = true;
        for (size_t i = 0; i < kPage && zero; ++i) zero = start[i] == 0;
        if (!zero) pages.push_back(page);
    }
    Word(out, uint32_t(pages.size()));
    for (size_t page : pages) {
        Word(out, uint32_t(page));
        Append(out, ram + page * kPage, kPage);
    }
}

void Export::Output(bool native, const ps2r::gs::DrawIr* draw) {
    if (!Enabled()) return;
    Bytes& draws = artifacts_.Output(native, "draw");
    Bytes& vertices = artifacts_.Output(native, "vertices");
    Word(draws, uint32_t(draw ? 1 : 0));
    Word(vertices, uint32_t(draw ? draw->verts.size() : 0));
    if (!draw) return;
    ++arms_[native].draws;
    ps2r::gs::DrawIr canonical = *draw;
    // Canonicalization, identical on both arms: untextured draws carry no
    // UV-vs-STQ register dependency (DrawFlush records the last written one,
    // the source intent none). Everything else is compared raw.
    if (!canonical.tex_enabled) canonical.tex.fst = false;
    const ps2r::gs::DrawRec rec = canonical.ToDrawRec();
    Append(draws, &rec, sizeof(rec));
    // Fields outside the DrawRec serialization that the oracle also compares.
    Word(draws, canonical.raster.dthe); Word(draws, canonical.raster.dimx);
    Word(draws, canonical.tex.mxl); Word(draws, uint8_t(canonical.tex.lcm));
    Word(draws, canonical.tex.l); Word(draws, canonical.tex.k);
    for (uint32_t tbp : canonical.tex.mip_tbp) Word(draws, tbp);
    for (uint32_t tbw : canonical.tex.mip_tbw) Word(draws, tbw);
    Append(vertices, canonical.verts.data(), canonical.verts.size() * sizeof(ps2r::gs::VertexRec));
}

void Export::Counters(bool native, const title::GraphicsService::Counters& service,
                      const ps2r::app::GraphicsStrip::Counters& owned) {
    if (!Enabled()) return;
    Arm& arm = arms_[native];
    for (size_t i = 0; i < service.variants.size(); ++i) {
        arm.service.variants[i].calls += service.variants[i].calls;
        arm.service.variants[i].replaced += service.variants[i].replaced;
        arm.service.variants[i].model_calls += service.variants[i].model_calls;
        arm.service.variants[i].interior_fallback += service.variants[i].interior_fallback;
    }
    arm.service.strip_calls += service.strip_calls;
    arm.service.replaced_producers += service.replaced_producers;
    arm.service.empty_producers += service.empty_producers;
    arm.service.owned_strip_producers += service.owned_strip_producers;
    arm.service.model_producers += service.model_producers;
    arm.service.fallback += service.fallback;
    arm.service.interior_fallback += service.interior_fallback;
    arm.service.output.completed_work += service.output.completed_work;
    arm.owned.captured += owned.captured;
    arm.owned.completed += owned.completed;
    arm.owned.failures += owned.failures;
}

void Export::Finish(int argc, char** argv) {
    if (!Enabled()) return;
    static constexpr const char* kVariantKeys[] = {
        "graphics_strip_plain", "graphics_strip_tinted", "graphics_strip_placed", "graphics_strip_placed_tinted"};
    for (bool native : {false, true}) {
        const Arm& arm = arms_[native];
        for (size_t i = 0; i < 4; ++i) artifacts_.Counter(native, kVariantKeys[i], Row(arm.service.variants[i]));
        artifacts_.Counter(native, "graphics_service",
            "strip_calls=" + std::to_string(arm.service.strip_calls) +
            ",replaced=" + std::to_string(arm.service.replaced_producers) +
            ",empty=" + std::to_string(arm.service.empty_producers) +
            ",owned_strips=" + std::to_string(arm.service.owned_strip_producers) +
            ",model_producers=" + std::to_string(arm.service.model_producers) +
            ",fallback=" + std::to_string(arm.service.fallback) +
            ",interior=" + std::to_string(arm.service.interior_fallback) +
            ",completed_work=" + std::to_string(arm.service.output.completed_work));
        artifacts_.Counter(native, "graphics_strip_output",
            "captured=" + std::to_string(arm.owned.captured) + ",completed=" + std::to_string(arm.owned.completed) +
            ",failed=" + std::to_string(arm.owned.failures));
        artifacts_.Counter(native, "oracle_runs", "runs=" + std::to_string(arm.runs) + ",draws=" + std::to_string(arm.draws));
    }
    artifacts_.Setting("hle.graphics_strips", "model", "all");
    artifacts_.Setting("rounding", "FE_TOWARDZERO", "FE_TOWARDZERO");
    artifacts_.Setting("input", "generated-fixture-ram-sparse-pages", "generated-fixture-ram-sparse-pages");
    artifacts_.Setting("consumer", "GraphicsScene/StripSceneFixture FakeBackend", "GraphicsScene/StripSceneFixture FakeBackend");
    artifacts_.Note("scope", "flat UI Strip producers, four source variants: canonical active DrawRec plus "
        "out-of-record raster/texture fields and vertices; no rasterizer/presentation claim");
    artifacts_.Note("canonicalization", "untextured draws: tex.fst zeroed on both arms (DrawFlush last-register "
        "dependency, no source intent); every other field raw");
    artifacts_.Note("input", "per recorded run: variant, a0/a1/a2, frame environment, sparse non-zero 4 KiB pages "
        "of the generated body's fixture RAM immediately before the body executes");
    artifacts_.Note("rounding", "FE_TOWARDZERO (the NEW caller contract) on both arms; the other three host modes "
        "are checked by the test but not exported");
    artifacts_.Note("excluded", "setup-then-visible continuation sequences are checked by the test but not exported: "
        "their second model call consumes the model's own packet writes, so the arms' images differ by design");
    artifacts_.Note("counter_provenance", "graphics_strip_*: GraphicsService::StripVariant per-variant census, "
        "published by hle/graphics/graphics_report.cpp; graphics_strip_output: GraphicsStrip owned shared-scene completion");
    artifacts_.Finish("StripSelfOracle", "strip-artifacts-v1", argc, argv);
}

}  // namespace strip_oracle
