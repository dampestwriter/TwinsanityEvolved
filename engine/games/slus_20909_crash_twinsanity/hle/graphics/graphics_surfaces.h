#pragma once

#include <memory>
#include <unordered_map>

#include "engine/render/backend.h"

namespace title {

// Allocation lifetimes shared by the retained renderer and typed Graphics.
// Both use this same Backend. Retiring a cache allocation cannot destroy it
// while recorded Graphics work holds Pin(). No pixels are copied or mirrored.
// Backend operations and CollectAfterQuiescence have one serialized context
// owner. Pins alone may be released on other threads.
class GraphicsSurfaces final : public ps2r::render::Backend {
public:
    explicit GraphicsSurfaces(ps2r::render::Backend& backend);
    // All cache owners and Graphics leases must be released first, after drain.
    ~GraphicsSurfaces() override;
    std::shared_ptr<const void> Pin(ps2r::render::SurfaceId surface) const;
    void CollectAfterQuiescence();
    size_t PendingRetirements() const;

    ps2r::render::Caps QueryCaps() const override;
    ps2r::render::SurfaceId CreateSurface(const ps2r::render::SurfaceDesc&) override;
    void DestroySurface(ps2r::render::SurfaceId) override;
    ps2r::render::SurfaceDesc DescribeSurface(ps2r::render::SurfaceId) const override;
    void Clear(ps2r::render::SurfaceId, ps2r::render::SurfaceId, uint32_t, float,
               uint32_t flags = ps2r::render::kClearColor | ps2r::render::kClearDepth) override;
    void Upload(ps2r::render::SurfaceId, const ps2r::render::Rect&, const void*, size_t) override;
    void UploadLevel(ps2r::render::SurfaceId, int, int, int, const void*) override;
    void FinishMipChain(ps2r::render::SurfaceId, int, bool) override;
    ps2r::render::DrawResult Draw(const ps2r::gs::DrawIr&, const ps2r::gs::TextureSource&,
                                 const ps2r::render::DrawParams&) override;
    void ClearStencil(ps2r::render::SurfaceId, uint8_t) override;
    void DrawConstQuad(ps2r::render::SurfaceId, ps2r::render::SurfaceId,
                       const ps2r::render::Rect&, uint32_t, bool,
                       const ps2r::render::StencilState&) override;
    void CopyRect(ps2r::render::SurfaceId, const ps2r::render::Rect&,
                  ps2r::render::SurfaceId, const ps2r::render::Rect&, uint32_t) override;
    void Readback(ps2r::render::SurfaceId, const ps2r::render::Rect&, void*, size_t) override;
    ps2r::render::FenceId BeginReadback(ps2r::render::SurfaceId, const ps2r::render::Rect&) override;
    bool PollReadback(ps2r::render::FenceId, void*, size_t) override;
    void Present(const ps2r::render::ScanoutDesc&) override;
    void ArmPresentCapture(bool) override;
    bool ReadPresented(std::vector<uint8_t>*, int*, int*) override;
    void BeginFrame() override;
    void EndFrame() override;

private:
    struct Allocation { ps2r::render::SurfaceId surface; bool retired = false; };
    ps2r::render::Backend& backend_;
    std::unordered_map<uint32_t, std::shared_ptr<Allocation>> allocations_;
};

}  // namespace title
