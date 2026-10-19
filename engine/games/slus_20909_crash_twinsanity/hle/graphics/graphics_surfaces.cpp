#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_surfaces.h"

#include <exception>
#include <stdexcept>

namespace title {
using namespace ps2r::render;

GraphicsSurfaces::GraphicsSurfaces(Backend& backend) : backend_(backend) {}
GraphicsSurfaces::~GraphicsSurfaces() {
    for (const auto& pair : allocations_) {
        if (pair.second.use_count() != 1) std::terminate();
        try { backend_.DestroySurface(pair.second->surface); }
        catch (...) { std::terminate(); }
    }
}
std::shared_ptr<const void> GraphicsSurfaces::Pin(SurfaceId surface) const {
    const auto found = allocations_.find(surface.id);
    return found == allocations_.end() || found->second->retired
        ? std::shared_ptr<const void>{} : found->second;
}
size_t GraphicsSurfaces::PendingRetirements() const {
    size_t count = 0;
    for (const auto& pair : allocations_) count += pair.second->retired;
    return count;
}
void GraphicsSurfaces::CollectAfterQuiescence() {
    for (auto it = allocations_.begin(); it != allocations_.end();) {
        if (it->second->retired && it->second.use_count() == 1) {
            backend_.DestroySurface(it->second->surface);
            it = allocations_.erase(it);
        } else ++it;
    }
}
Caps GraphicsSurfaces::QueryCaps() const { return backend_.QueryCaps(); }
SurfaceId GraphicsSurfaces::CreateSurface(const SurfaceDesc& description) {
    const auto surface = backend_.CreateSurface(description);
    if (!surface.id) return surface;
    if (allocations_.count(surface.id))
        throw std::runtime_error("Backend reused a pinned Graphics surface");
    try {
        allocations_.emplace(surface.id, std::make_shared<Allocation>(Allocation{surface, false}));
    } catch (...) {
        backend_.DestroySurface(surface);
        throw;
    }
    return surface;
}
void GraphicsSurfaces::DestroySurface(SurfaceId surface) {
    const auto found = allocations_.find(surface.id);
    if (found == allocations_.end()) return;
    found->second->retired = true;
    // No pin means no new lifetime to fence: preserve the caller's existing
    // destruction ordering. A pinned allocation waits for explicit collection.
    if (found->second.use_count() == 1) {
        backend_.DestroySurface(surface);
        allocations_.erase(found);
    }
}
SurfaceDesc GraphicsSurfaces::DescribeSurface(SurfaceId surface) const {
    return allocations_.count(surface.id) ? backend_.DescribeSurface(surface) : SurfaceDesc{};
}
void GraphicsSurfaces::Clear(SurfaceId color, SurfaceId depth, uint32_t rgba, float z, uint32_t flags) {
    backend_.Clear(color, depth, rgba, z, flags);
}
void GraphicsSurfaces::Upload(SurfaceId surface, const Rect& rect, const void* bytes, size_t stride) {
    backend_.Upload(surface, rect, bytes, stride);
}
void GraphicsSurfaces::UploadLevel(SurfaceId surface, int level, int w, int h, const void* bytes) {
    backend_.UploadLevel(surface, level, w, h, bytes);
}
void GraphicsSurfaces::FinishMipChain(SurfaceId surface, int levels, bool synthesize) {
    backend_.FinishMipChain(surface, levels, synthesize);
}
DrawResult GraphicsSurfaces::Draw(const ps2r::gs::DrawIr& draw, const ps2r::gs::TextureSource& texture,
                                 const DrawParams& params) {
    return backend_.Draw(draw, texture, params);
}
void GraphicsSurfaces::ClearStencil(SurfaceId surface, uint8_t value) {
    backend_.ClearStencil(surface, value);
}
void GraphicsSurfaces::DrawConstQuad(SurfaceId color, SurfaceId stencil, const Rect& rect,
                                   uint32_t rgba, bool alpha, const StencilState& state) {
    backend_.DrawConstQuad(color, stencil, rect, rgba, alpha, state);
}
void GraphicsSurfaces::CopyRect(SurfaceId src, const Rect& from, SurfaceId dst,
                              const Rect& to, uint32_t transform) {
    backend_.CopyRect(src, from, dst, to, transform);
}
void GraphicsSurfaces::Readback(SurfaceId surface, const Rect& rect, void* out, size_t stride) {
    backend_.Readback(surface, rect, out, stride);
}
FenceId GraphicsSurfaces::BeginReadback(SurfaceId surface, const Rect& rect) {
    return backend_.BeginReadback(surface, rect);
}
bool GraphicsSurfaces::PollReadback(FenceId fence, void* out, size_t stride) {
    return backend_.PollReadback(fence, out, stride);
}
void GraphicsSurfaces::Present(const ScanoutDesc& description) { backend_.Present(description); }
void GraphicsSurfaces::ArmPresentCapture(bool armed) { backend_.ArmPresentCapture(armed); }
bool GraphicsSurfaces::ReadPresented(std::vector<uint8_t>* rgba, int* w, int* h) {
    return backend_.ReadPresented(rgba, w, h);
}
void GraphicsSurfaces::BeginFrame() { backend_.BeginFrame(); }
void GraphicsSurfaces::EndFrame() { backend_.EndFrame(); }

}  // namespace title
