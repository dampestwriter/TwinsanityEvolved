#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"
#include "games/slus_20909_crash_twinsanity/tests/title/text/text_oracle.h"

#include <cfenv>
#include <map>

#include "engine/gs/state/draw_flush.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_service.h"
#include "games/slus_20909_crash_twinsanity/vu1_library/sprite_text.h"
#include "tests/lib/capture_sink.h"

namespace strip_oracle {
namespace memory { std::array<uint8_t, 0x400000> bytes{}; }
uint64_t generated_calls = 0, typesetter_calls = 0;
std::vector<uint32_t> materials;
void ObserveGenerated(uint32_t) { ++generated_calls; }
void ExternalCall(uint32_t address, CpuContext& c) {
    const uint32_t object = c.cpuRegs.GPR.r[4].UL[0];
    if (address == 0x1815c0 && object <= 0x100) {
        c.cpuRegs.GPR.r[2].UL[0] = object == 0x60 ? 0x1e0000 : 0x1e0100; return;
    }
    if (address == 0x1c0190) {
        // Allocation/list insertion only. The actual NEW BeginTextPacket and
        // Font::End write the chains, glyph reference and page replay refs.
        materials.push_back(object);
        const uint32_t tag = memory::read<uint32_t>(0x323310);
        if (tag < 0x230000 || tag >= 0x260000) std::abort();
        memory::write<uint32_t>(object + 0x28, tag);
        memory::write<uint32_t>(object + 0x30, tag);
        memory::write<uint32_t>(object + 0x38, 0);
        memory::write<uint32_t>(tag, 0x20000000);
        memory::write<uint32_t>(tag + 4, tag);
        memory::write<uint32_t>(0x323310, tag + 16);
        return;
    }
    std::fprintf(stderr, "unexpected text oracle callee %x (a0=%x)\n", address, object);
    std::abort();
}
}
namespace {
namespace ram = strip_oracle::memory;
void Require(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "TextOracle: %s\n", what); std::abort(); }
}
CpuContext Context(uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0) {
    CpuContext c{};
    c.cpuRegs.GPR.r[28].UL[0] = 0x3114f0; c.cpuRegs.GPR.r[29].UL[0] = 0x2e0000;
    c.cpuRegs.GPR.r[4].UL[0] = a0; c.cpuRegs.GPR.r[5].UL[0] = a1; c.cpuRegs.GPR.r[6].UL[0] = a2;
    c.vuRegs.VF[0].w = 1; return c;
}
void Settings(ps2r::gs::GsRegisters& regs, uint32_t address, uint32_t count) {
    for (uint32_t n = 0; n < count; ++n)
        Require(regs.Write(uint8_t(ram::read<uint64_t>(address + n * 16 + 8)),
                           ram::read<uint64_t>(address + n * 16)), "settings register");
}
ps2r::gs::GsRegisters Registers(uint32_t shader, uint32_t material,
                               const title::MaterialState::Environment& env) {
    const bool shared = ram::read<uint32_t>(material + 0x60) == 2;
    auto c = shared ? Context() : Context(0x160000);
    strip_oracle::RunGenerated(shared ? 0x19b480 : 0x19ef90, c);
    c = Context(shader); strip_oracle::RunGenerated(0x1ce778, c);
    ps2r::gs::GsRegisters regs;
    regs.env.colclamp = env.colour_clamp; regs.env.pabe = env.per_pixel_alpha;
    regs.env.dthe = env.dither; regs.env.dimx = env.dither_matrix; regs.env.scanmsk = env.scan_mask;
    regs.env.fogcol_r = env.fog_r; regs.env.fogcol_g = env.fog_g; regs.env.fogcol_b = env.fog_b;
    regs.ctx[0].alpha = {2, 2, 2, 2, 255};
    Settings(regs, shared ? 0x1e0020 : 0x1e2020, 3);
    Settings(regs, 0x1e0110, ram::read<uint32_t>(shader + 0x5c) - 1);
    // SAME synthetic loaded texture header, independently checked by the
    // original-byte TextureAssets oracle. No replacement sampling calculation.
    regs.ctx[0].tex0 = {0, 128, 19, 7, 7, 1, 0, 0, 0, 0, 0, 0};
    return regs;
}
uint32_t Word(const std::vector<uint8_t>& bytes, size_t at) {
    uint32_t value; std::memcpy(&value, bytes.data() + at, 4); return value;
}
uint64_t Long(const std::vector<uint8_t>& bytes, size_t at) {
    uint64_t value; std::memcpy(&value, bytes.data() + at, 8); return value;
}
void Deliver(const Vu1HleOutput& generated, ps2r::gs::GsRegisters& regs, ps2r::gs::VertexKick& kick,
             ps2r::gs::DrawFlush& flush) {
    for (const auto& packet : generated.kicks) {
        // Transport the one source-declared UV/RGBAQ/XYZ/UV/XYZ template.
        // Geometry and sprite assembly come from actual NEW, not this helper.
        const uint32_t count = Word(packet, 0) & 0x7fff;
        Require(count <= 20 && packet.size() == 16 + size_t(count) * 80, "glyph packet extent");
        Require((Word(packet, 8) & 0xfffff) == 0x53513, "glyph packet descriptors");
        Require(regs.Write(0, 6), "sprite primitive"); kick.OnPrimWritten();
        for (uint32_t n = 0; n < count; ++n) {
            const size_t at = 16 + size_t(n) * 80;
            const auto uv = [&](size_t offset) {
                Require(regs.Write(3, uint64_t(Word(packet, offset) & 0x3fff) |
                                   uint64_t(Word(packet, offset + 4) & 0x3fff) << 16), "UV");
            };
            uv(at);
            uint64_t rgba = uint64_t{0x3f800000} << 32;
            for (unsigned channel = 0; channel < 4; ++channel)
                rgba |= uint64_t(Word(packet, at + 16 + channel * 4) & 255) << (channel * 8);
            Require(regs.Write(1, rgba), "RGBAQ");
            kick.PackedXyz(true, Long(packet, at + 32), Long(packet, at + 40));
            uv(at + 48);
            kick.PackedXyz(true, Long(packet, at + 64), Long(packet, at + 72));
        }
        const bool emitted = flush.Flush(ps2r::gs::DrawFlush::FlushKind::GifEop);
        Require(emitted == (count != 0), "glyph draw extent");
    }
}
std::vector<ps2r::gs::DrawIr> GlyphDraws(uint32_t material, uint32_t font,
                                 const title::MaterialState::Environment& environment) {
    constexpr uint32_t node = 20, top = 350, packets = 0x2e0000 - 0x440 + 0x10;
    const uint32_t shader = ram::read<uint32_t>(material);
    auto regs = Registers(shader, material, environment);
    ps2r::gs::VertexKick kick(&regs);
    ps2r::gs::CaptureSink sink;
    ps2r::gs::DrawFlush flush(&regs, &kick); flush.SetSink(&sink);
    // Actual NEW ShaderType0DPacket supplies corner, descriptor and page mask.
    ram::write<uint32_t>(0x1e1f00, node);
    auto c = Context(shader, 0x1e1000, 0x1e1f00);
    strip_oracle::RunGenerated(0x1ceb10, c);
    Require(c.cpuRegs.GPR.r[2].UL[0] == 0x1e10a0, "font parameter extent");
    std::array<uint8_t, kVu1HleDataSize> data{}, micro{};
    std::memcpy(data.data() + node * 16, ram::bytes.data() + 0x1e1010, 9 * 16);
    QwPtr memory{data.data()};
    memory.SetW(node + 1, 0, 800); memory.SetW(node + 2, 0, 920);
    const uint32_t count = ram::read<uint32_t>(font), glyphs = ram::read<uint32_t>(font + 8);
    Require(count && count <= 256, "font glyph count");
    std::memcpy(data.data() + 54 * 16, ram::bytes.data() + glyphs, count * 16);
    VUregs vu{};
    vu.VF[0].w = 1; vu.VI[14] = node; vu.VI[13] = node + 1;
    std::memcpy(&vu.VF[1], ram::bytes.data() + 0x1e1040, 16);
    std::memcpy(&vu.VF[2], ram::bytes.data() + 0x1e1050, 16);
    const uint32_t groups = ram::read<uint32_t>(packets);
    Require(groups <= 128, "font source packet count");
    for (uint32_t group = 0; group < groups; ++group) {
        const uint32_t tag = ram::read<uint32_t>(packets + 0x204 + group * 4);
        const uint32_t lines = ram::read<uint32_t>(tag + 0x28);
        Require(lines && lines <= 64, "text group lines");
        std::memcpy(data.data() + top * 16, ram::bytes.data() + tag + 16, (lines + 2) * 16);
        uint32_t glyph_count = 0;
        for (uint32_t line = 0; line < lines; ++line)
            glyph_count += ram::read<uint32_t>(tag + 0x38 + line * 16) * 4;
        Require(glyph_count <= 0x260, "text group glyph bound");
        const uint32_t bytes = tag + (lines + 3) * 16 + 4;
        for (uint32_t index = 0; index < glyph_count; ++index)
            memory.SetW(top + 2 + lines + index / 4, index % 4, ram::read<uint8_t>(bytes + index));
        Vu1HleOutput output;
        ++strip_oracle::typesetter_calls;
        Require(SpriteText::Run({micro.data(), data.data(), group ? 0x1980u : 0x1748u,
                                 top, top, 0, 0, &vu, 0}, output), "current NEW typesetter");
        Deliver(output, regs, kick, flush);
        vu = output.regs;
        for (const auto& write : output.data_writes)
            std::memcpy(data.data() + write.addr, write.bytes.data(), write.bytes.size());
    }
    return std::move(sink.draws);
}
}
std::vector<TextOracle::Page> TextOracle::Run(TextFixture& f, const title::MaterialState::Environment& env) {
    const int rounding = std::fegetround();
    std::fesetround(FE_TOWARDZERO);
    std::copy(f.textures.ram.begin(), f.textures.ram.end(), ram::bytes.begin());
    strip_oracle::materials.clear();
    ram::write<uint32_t>(0x323310, 0x230000);
    auto c = Context(); strip_oracle::RunGenerated(0x1da2d8, c);
    c = Context(0, TextFixture::queue);
    // The generated DrawFontTexts runs through a model-mode TextService so the
    // model arm's census is the catalog's Graphics.text.model_calls increment.
    {
        title::TextService::Hooks hooks;
        hooks.enabled = false;
        title::TextService service(f.textures.bindings, hooks);
        std::map<uint32_t, title::TextService::Body> installed;
        Require(service.Install([](uint32_t entry) -> title::TextService::Body {
            return [entry](CpuContext& ctx, uint32_t) { strip_oracle::RunGenerated(entry, ctx); };
        }, [&](uint32_t entry, title::TextService::Body body) { installed[entry] = std::move(body); return true; }),
            "model-mode TextService installed");
        installed.at(title::TextIntent::kDrawFontTexts)(c, title::TextIntent::kDrawFontTexts);
        Require(service.Counts().model_calls == 1 && service.Counts().replaced == 0, "model-mode census");
        model_census_.model_calls += service.Counts().model_calls;
        model_census_.calls += service.Counts().calls;
    }
    Require(ram::read<uint32_t>(0x30a7d0) == 0, "completed text writer");
    std::vector<TextOracle::Page> pages;
    for (uint32_t material : strip_oracle::materials)
        pages.push_back({material, GlyphDraws(material, TextFixture::font, env)});
    std::fesetround(rounding);
    return pages;
}
uint64_t TextOracle::Calls() { return strip_oracle::generated_calls + strip_oracle::typesetter_calls; }
title::TextService::Counters TextOracle::model_census_;
title::TextService::Counters TextOracle::ModelCensus() { return model_census_; }
