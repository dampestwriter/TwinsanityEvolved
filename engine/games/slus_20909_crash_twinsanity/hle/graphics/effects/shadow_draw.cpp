#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_draw.h"

#include <cmath>
#include <utility>

namespace title {
namespace {
using Draw = ps2r::gs::DrawIr;
using Vertex = ps2r::gs::VertexRec;
using Step = ShadowDraw::Step;

Draw State(const ShadowDraw::Frame& frame, bool half) {
    Draw draw;
    draw.src_frame = frame.number;
    draw.prim_type = 6;
    draw.use_fst = true;
    draw.raster.frame.fbw = half ? frame.width / 2 : frame.width;
    draw.raster.frame.psm = half ? 0x0a : 0;
    draw.raster.zbuf.zmsk = true;
    draw.raster.test.zte = true;
    draw.raster.test.ztst = 1;
    draw.raster.colclamp = true;
    draw.raster.scissor.x1 = uint16_t((half ? frame.width / 2 : frame.width) - 1);
    draw.raster.scissor.y1 = uint16_t((half ? frame.height / 2 : frame.height) - 1);
    return draw;
}
void Rect(Draw& draw, float left, float right, float bottom,
          float u0, float u1, float v1, uint8_t colour, uint8_t alpha) {
    Vertex a{}, b{}, c{}, d{};
    a.x = c.x = left;
    b.x = d.x = right;
    a.y = b.y = 0;
    c.y = d.y = bottom;
    a.uv_u = c.uv_u = uint16_t(u0 * 16) & 0x3fff;
    b.uv_u = d.uv_u = uint16_t(u1 * 16) & 0x3fff;
    a.uv_v = b.uv_v = 8;
    c.uv_v = d.uv_v = uint16_t(v1 * 16) & 0x3fff;
    for (auto* v : {&a, &b, &c, &d}) {
        v->z = 65535;
        v->q = 1;
        v->r = v->g = v->b = colour;
        v->a = alpha;
        v->draw_kick = 1;
    }
    draw.verts.insert(draw.verts.end(), {a, b, c, c, b, d});
    draw.vertex_count = uint32_t(draw.verts.size());
}
Draw Strips(const ShadowDraw::Frame& f, bool half, uint32_t step,
            uint32_t source_step, uint32_t source_height, uint8_t colour, uint8_t alpha) {
    Draw draw = State(f, half);
    const uint32_t width = half ? f.width / 2 : f.width;
    const uint32_t height = half ? f.height / 2 : f.height;
    for (uint32_t x = 0, source = 0; x < width; x += step, source += source_step)
        Rect(draw, float(x), float(x + step), float(height),
             float(source) + 0.5f, float(source + source_step) + 0.5f,
             float(source_height) + 0.5f, colour, alpha);
    return draw;
}
void Textured(Draw& draw, uint8_t format, uint8_t power, uint32_t width) {
    draw.tex_enabled = true;
    draw.tex.fst = true;
    draw.tex.psm = format;
    draw.tex.tw = draw.tex.th = power;
    draw.tex.tbw = width;
    draw.tex.tcc = 1;
    draw.tex.wms = draw.tex.wmt = 1;
    draw.tex.texa_ta0 = 0x80;
    draw.tex.texa_ta1 = 0;
}
}  // namespace

ShadowDraw::Result ShadowDraw::Build(const Frame& frame,
    const std::vector<Draw>& volumes, std::vector<Command>& out) {
    // The source's integer strip loops cover complete 64-pixel groups.
    if (!frame.width || frame.width > 2048 || frame.width % 64 ||
        !frame.height || frame.height > 2048 || frame.height % 2)
        return Result::InvalidFrame;
    size_t vertices = 0;
    for (const auto& volume : volumes) {
        if (volume.verts.empty() || volume.verts.size() % 3)
            return Result::InvalidVolume;
        if (volume.verts.size() > kMaxVertices - vertices) return Result::LimitExceeded;
        vertices += volume.verts.size();
        const auto colour = volume.verts.front().r;
        if (colour != 8 && colour != 248) return Result::InvalidVolume;
        for (const auto& v : volume.verts)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
                v.z < 0 || double(v.z) > 4294967296.0 ||
                v.r != colour || v.g != colour || v.b != colour)
                return Result::InvalidVolume;
    }
    std::vector<Command> commands;
    commands.reserve(volumes.size() + 6);
    auto draw = Strips(frame, true, 32, 32, frame.height / 2, 0x78, 0x78);
    commands.push_back({Step::Clear, std::move(draw)});
    draw = Strips(frame, true, 16, 32, frame.height, 0x80, 0x80);
    draw.raster.frame.fbmsk = 0x00ffffff;
    Textured(draw, 0, 10, frame.width);
    commands.push_back({Step::SeedAlpha, std::move(draw)});
    draw = Strips(frame, true, 16, 32, frame.height, 0x80, 0x80);
    draw.raster.frame.psm = 0x30;
    Textured(draw, 0x30, 10, frame.width);
    commands.push_back({Step::CopyDepth, std::move(draw)});
    draw = Strips(frame, true, 32, 32, frame.height / 2, 0x80, 0);
    draw.raster.frame.psm = 0;
    draw.alpha_blend = true;
    draw.raster.alpha = {0, 1, 2, 2, 0x80};
    Textured(draw, 0x1b, 10, frame.width / 2);
    draw.tex.tfx = 3;
    commands.push_back({Step::InvertDepth, std::move(draw)});
    for (const auto& volume : volumes) {
        draw = State(frame, true);
        draw.prim_type = 4;
        draw.alpha_blend = true;
        draw.raster.alpha = {0, 2, 2, 1, 0x80};
        draw.raster.frame.fbmsk = 0xff000000;
        draw.raster.colclamp = false;
        draw.raster.test.date = true;
        draw.raster.test.ztst = 2;
        draw.verts = volume.verts;
        draw.vertex_count = uint32_t(draw.verts.size());
        commands.push_back({Step::Volume, std::move(draw)});
    }
    draw = Strips(frame, true, 32, 32, frame.height / 2, 0, 0);
    draw.raster.frame.fbmsk = 0xff808080;
    commands.push_back({Step::Quantize, std::move(draw)});
    draw = Strips(frame, false, 32, 16, frame.height / 2, 0x80, 0x80);
    Textured(draw, 0x0a, 9, frame.width / 2);
    draw.tex.mmin = 1;
    draw.tex.mmag = draw.tex.mmin;
    draw.alpha_blend = true;
    draw.raster.alpha = {2, 0, 2, 1, 0x20};
    commands.push_back({Step::Composite, std::move(draw)});
    out = std::move(commands);
    return Result::Ok;
}

uint8_t ShadowDraw::CounterByte(int32_t winding) {
    return uint8_t((15u + uint32_t(winding)) * 8u);
}
uint8_t ShadowDraw::QuantizedByte(int32_t winding) {
    return CounterByte(winding) & 0x80 ? 0x84 : 0;
}
uint8_t ShadowDraw::Darken(uint8_t scene, uint8_t mask) {
    // The source integer blend subtracts one quarter, saturating at zero.
    const int32_t result = int32_t(scene) - int32_t(mask) / 4;
    return uint8_t(result < 0 ? 0 : result);
}
}  // namespace title
