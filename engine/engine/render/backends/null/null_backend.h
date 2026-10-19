// render/backends/null/null_backend.h -- NullBackend: the render::Backend that rasterises
// nothing.
//
// Every entry of the contract is honoured in shape and counted, no pixel is ever produced:
// surfaces are descriptors in a table (CreateSurface / DescribeSurface / DestroySurface work),
// uploads, clears, copies and draws are accepted and tallied (a Draw of three or more vertices
// reports drew = true with the bounding box of its vertices so a caller's coherence tail runs
// unchanged), readbacks hand back zero bytes, presents are counted, no window image is ever
// held. For a host whose graphics API has no adapter yet (a console target's first boot) and
// for the layers above the interface under test without a context. No API header, no
// platform dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "engine/render/backend.h"

namespace ps2r {
namespace render {

class NullBackend final : public Backend {
public:
    struct Census {
        uint64_t surfaces_created = 0, surfaces_destroyed = 0;
        uint64_t clears = 0, uploads = 0, level_uploads = 0, mip_finishes = 0;
        uint64_t draws = 0, draws_refused = 0, triangles = 0;
        uint64_t stencil_clears = 0, const_quads = 0, stencil_marks = 0;
        uint64_t copies = 0, readbacks = 0, presents = 0;
        uint64_t frames = 0, draw_runs = 0;
    };

    Caps QueryCaps() const override;
    SurfaceId CreateSurface(const SurfaceDesc& desc) override;
    void DestroySurface(SurfaceId id) override;
    SurfaceDesc DescribeSurface(SurfaceId id) const override;
    void Clear(SurfaceId color, SurfaceId depth, uint32_t rgba, float z, uint32_t flags) override;
    void Upload(SurfaceId id, const Rect& rect, const void* bytes, size_t stride) override;
    void UploadLevel(SurfaceId id, int level, int w, int h, const void* rgba) override;
    void FinishMipChain(SurfaceId id, int authored_levels, bool synthesize_tail) override;
    DrawResult Draw(const gs::DrawIr& ir, const gs::TextureSource& tex, const DrawParams& params) override;
    void ClearStencil(SurfaceId stencil, uint8_t value) override;
    void DrawConstQuad(SurfaceId color, SurfaceId stencil, const Rect& scissor, uint32_t rgba, bool write_alpha,
                       const StencilState& st) override;
    bool MarkStencilWhereOpaque(SurfaceId color, SurfaceId stencil, uint8_t bits) override;
    bool ReadbackStencil(SurfaceId stencil, const Rect& rect, uint8_t* out) override;
    void CopyRect(SurfaceId src, const Rect& src_rect, SurfaceId dst, const Rect& dst_rect,
                  uint32_t transform) override;
    void Readback(SurfaceId id, const Rect& rect, void* out, size_t stride) override;
    FenceId BeginReadback(SurfaceId id, const Rect& rect) override;
    bool PollReadback(FenceId fence, void* out, size_t stride) override;
    void Present(const ScanoutDesc& desc) override;
    void SetWindowSize(int w, int h) override;
    void BeginFrame() override;
    void EndFrame() override;
    void BeginDrawRun() override;
    void EndDrawRun() override;

    const Census& Stats() const { return census_; }
    size_t SurfacesAlive() const { return surfaces_.size(); }
    int WindowWidth() const { return window_w_; }
    int WindowHeight() const { return window_h_; }

private:
    bool Has(SurfaceId id) const { return id.id != 0 && surfaces_.count(id.id) != 0; }

    std::unordered_map<uint32_t, SurfaceDesc> surfaces_;
    uint32_t next_surface_ = 1;
    struct PendingReadback {
        size_t rows = 0;
        size_t row_bytes = 0;
    };
    std::unordered_map<uint64_t, PendingReadback> readbacks_;
    uint64_t next_fence_ = 1;
    int window_w_ = 0, window_h_ = 0;
    Census census_;
};

}  // namespace render
}  // namespace ps2r
