// render/backends/null/null_backend.cpp -- NullBackend: the contract honoured in shape, nothing rasterised.
#include "engine/render/backends/null/null_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/texsrc/texture_source.h"

namespace ps2r {
namespace render {

namespace {

size_t BytesPerTexel(SurfaceFormat f) {
    switch (f) {
        case SurfaceFormat::R8: return 1;
        case SurfaceFormat::Depth32f: return 4;
        default: return 4;
    }
}

}  // namespace

Caps NullBackend::QueryCaps() const {
    Caps c;
    c.max_texture_size = 4096;
    c.staging_readback = false;
    c.reverse_subtract = true;
    c.depth_sample = false;
    c.texture_barrier = false;
    c.compute = false;
    return c;
}

SurfaceId NullBackend::CreateSurface(const SurfaceDesc& desc) {
    if (desc.width == 0 || desc.height == 0) return SurfaceId{};
    const uint32_t id = next_surface_++;
    surfaces_[id] = desc;
    census_.surfaces_created++;
    return SurfaceId{id};
}

void NullBackend::DestroySurface(SurfaceId id) {
    if (surfaces_.erase(id.id)) census_.surfaces_destroyed++;
}

SurfaceDesc NullBackend::DescribeSurface(SurfaceId id) const {
    auto it = surfaces_.find(id.id);
    return it == surfaces_.end() ? SurfaceDesc{} : it->second;
}

void NullBackend::Clear(SurfaceId color, SurfaceId depth, uint32_t, float, uint32_t) {
    if (Has(color) || Has(depth)) census_.clears++;
}

void NullBackend::Upload(SurfaceId id, const Rect&, const void*, size_t) {
    if (Has(id)) census_.uploads++;
}

void NullBackend::UploadLevel(SurfaceId id, int, int, int, const void*) {
    if (Has(id)) census_.level_uploads++;
}

void NullBackend::FinishMipChain(SurfaceId id, int, bool) {
    if (Has(id)) census_.mip_finishes++;
}

// The bounding boxes are the GL arm's rules in outline: the triangle-complete vertices'
// box, extended over the tail; the write rect is that box within the scissor and the target.
DrawResult NullBackend::Draw(const gs::DrawIr& ir, const gs::TextureSource&, const DrawParams& p) {
    DrawResult r;
    const size_t n = ir.verts.size();
    if (n < 3 || !Has(p.color)) {
        census_.draws_refused++;
        return r;
    }
    const size_t tri_verts = (n / 3) * 3;
    float tri[4] = {ir.verts[0].x, ir.verts[0].y, ir.verts[0].x, ir.verts[0].y};
    for (size_t i = 1; i < tri_verts; ++i) {
        tri[0] = std::min(tri[0], ir.verts[i].x);
        tri[1] = std::min(tri[1], ir.verts[i].y);
        tri[2] = std::max(tri[2], ir.verts[i].x);
        tri[3] = std::max(tri[3], ir.verts[i].y);
    }
    float all[4] = {tri[0], tri[1], tri[2], tri[3]};
    for (size_t i = tri_verts; i < n; ++i) {
        all[0] = std::min(all[0], ir.verts[i].x);
        all[1] = std::min(all[1], ir.verts[i].y);
        all[2] = std::max(all[2], ir.verts[i].x);
        all[3] = std::max(all[3], ir.verts[i].y);
    }
    r.drew = true;
    r.bbox_valid = true;
    for (int i = 0; i < 4; ++i) {
        r.bbox_tri[i] = tri[i];
        r.bbox_all[i] = all[i];
    }
    r.alt16_route = p.alt16_route;
    const auto& sc = ir.raster.scissor;
    int x0 = std::max(static_cast<int>(std::floor(all[0])), static_cast<int>(sc.x0));
    int y0 = std::max(static_cast<int>(std::floor(all[1])), static_cast<int>(sc.y0));
    int x1 = std::min(static_cast<int>(std::ceil(all[2])), static_cast<int>(sc.x1) + 1);
    int y1 = std::min(static_cast<int>(std::ceil(all[3])), static_cast<int>(sc.y1) + 1);
    x0 = std::max(0, std::min(x0, p.target_w));
    y0 = std::max(0, std::min(y0, p.target_h));
    x1 = std::max(0, std::min(x1, p.target_w));
    y1 = std::max(0, std::min(y1, p.target_h));
    if (x1 > x0 && y1 > y0) r.write_rect = Rect{x0, y0, x1 - x0, y1 - y0};
    census_.draws++;
    census_.triangles += tri_verts / 3;
    return r;
}

void NullBackend::ClearStencil(SurfaceId stencil, uint8_t) {
    if (Has(stencil)) census_.stencil_clears++;
}

void NullBackend::DrawConstQuad(SurfaceId, SurfaceId stencil, const Rect& scissor, uint32_t, bool,
                                const StencilState&) {
    if (Has(stencil) && scissor.w > 0 && scissor.h > 0) census_.const_quads++;
}

bool NullBackend::MarkStencilWhereOpaque(SurfaceId color, SurfaceId stencil, uint8_t) {
    if (!Has(color) || !Has(stencil)) return false;
    census_.stencil_marks++;
    return true;
}

bool NullBackend::ReadbackStencil(SurfaceId, const Rect&, uint8_t*) { return false; }

void NullBackend::CopyRect(SurfaceId src, const Rect&, SurfaceId dst, const Rect&, uint32_t) {
    if (Has(src) && Has(dst)) census_.copies++;
}

// Zero bytes in the attachment's layout: a caller that measures a frame measures black.
void NullBackend::Readback(SurfaceId id, const Rect& rect, void* out, size_t stride) {
    if (!out || rect.w <= 0 || rect.h <= 0) return;
    const SurfaceDesc d = DescribeSurface(id);
    if (!Has(id)) return;
    const size_t row_bytes = static_cast<size_t>(rect.w) * BytesPerTexel(d.format);
    const size_t step = stride ? stride : row_bytes;
    auto* dst = static_cast<uint8_t*>(out);
    for (int y = 0; y < rect.h; ++y) std::memset(dst + static_cast<size_t>(y) * step, 0, row_bytes);
    census_.readbacks++;
}

FenceId NullBackend::BeginReadback(SurfaceId id, const Rect& rect) {
    if (!Has(id) || rect.w <= 0 || rect.h <= 0) return FenceId{};
    const uint64_t fence = next_fence_++;
    readbacks_[fence] = PendingReadback{static_cast<size_t>(rect.h),
                                        static_cast<size_t>(rect.w) * BytesPerTexel(DescribeSurface(id).format)};
    return FenceId{fence};
}

bool NullBackend::PollReadback(FenceId fence, void* out, size_t stride) {
    auto it = readbacks_.find(fence.id);
    if (it == readbacks_.end()) return false;
    if (out) {
        const size_t step = stride ? stride : it->second.row_bytes;
        auto* dst = static_cast<uint8_t*>(out);
        for (size_t y = 0; y < it->second.rows; ++y) std::memset(dst + y * step, 0, it->second.row_bytes);
    }
    readbacks_.erase(it);
    census_.readbacks++;
    return true;
}

void NullBackend::Present(const ScanoutDesc& desc) {
    if (desc.source.id != 0 && !Has(desc.source)) return;   // the contract's refusal: a missing nonzero source
    census_.presents++;
}

void NullBackend::SetWindowSize(int w, int h) {
    window_w_ = w;
    window_h_ = h;
}

void NullBackend::BeginFrame() {
    census_.frames++;
    BeginDrawRun();
}

void NullBackend::EndFrame() { EndDrawRun(); }
void NullBackend::BeginDrawRun() { census_.draw_runs++; }
void NullBackend::EndDrawRun() {}

}  // namespace render
}  // namespace ps2r
