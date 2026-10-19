#pragma once

#include "games/slus_20909_crash_twinsanity/hle/fmv/movie_graphics.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_pass.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_assets.h"

namespace ps2r::app {

// Render-owner half of GraphicsProducers. Inputs already own their source
// frame, geometry and image generation. No guest memory or model state is read.
class GraphicsProducerDraws {
public:
    using Frame = ::title::GraphicsScene::Frame;
    struct AssetDraw {
        gs::DrawIr draw;
        ::title::TextureAssets::View texture;
    };
    GraphicsProducerDraws(::title::GraphicsResources&, ::title::GraphicsRenderer&,
                          ::title::GraphicsScene&, ::title::TextureAssets&,
                          bool synthesize_tail);
    ~GraphicsProducerDraws();
    GraphicsProducerDraws(const GraphicsProducerDraws&) = delete;
    GraphicsProducerDraws& operator=(const GraphicsProducerDraws&) = delete;
    // `why` (optional) names a refusal: kBindRefused (the owned scene would not
    // bind the frame's target), kUploadRefused (the asset's lease/upload), or
    // kDrawRefused | GraphicsScene::Result of the first draw the scene declined.
    static constexpr uint32_t kBindRefused = 0x10, kUploadRefused = 0x20, kDrawRefused = 0x40;
    bool Assets(const Frame&, const std::vector<AssetDraw>&, uint32_t* why = nullptr);
    bool UploadMovie(const ::title::MovieGraphics::ImageLease&);
    bool Movie(const Frame&, const gs::DrawIr&, const ::title::MovieGraphics::ImageLease&);
    bool Sky(const Frame&, const ::title::SkyPass::Output&);
    // After all ordered callbacks drain. Releases only this helper's cached
    // handles; backend destruction stays with GraphicsResources' fenced owner.
    void ResetAfterQuiescence();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ps2r::app
