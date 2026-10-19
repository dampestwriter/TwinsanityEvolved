#pragma once

#include <array>

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics.h"

namespace title {

// Named allocation graph for the first scene/UI and sky-composite passes.
// Source: TR frame.cpp SetUpRenderTarget and sky.cpp FinishSky. SceneCopy is
// the backend's explicit destination-color read needed by FBA/DATE blending.
// Other frame/shadow/fog/asset families must add their own source-checked views.
// No numeric address is a target identity and no alias is inferred.
class GraphicsTargets {
public:
    enum class Name { SceneColor, SceneDepth, SceneCopy, SkyColor, SkyDepth, SkyCopy,
                      DisplayColor, DisplayDepth, DisplayCopy, Count };
    enum class Result { Ok, InvalidName, InvalidBinding, MissingTarget, Feedback,
                        UnsupportedDraw, QueueRejected };
    // The ordered consumer contract for GraphicsRenderer. Texture is optional;
    // color/depth/copy are explicit even when a particular draw doesn't use all.
    enum Slot : size_t { Color = 0, Depth = 1, Copy = 2, Texture = 3 };
    explicit GraphicsTargets(Graphics& queue);
    Result Bind(Name name, const Graphics::Handle& handle);
    void Unbind(Name name);
    void Reset();
    // Resolves names NOW, so rebinding/resizing after recording cannot redirect
    // an older draw to new allocations. The queue owns the resulting leases.
    // This bounded route draws to SceneColor/SceneDepth/SceneCopy and optionally
    // samples SkyColor. It does not stand for a general material/alias resolver.
    Result Record(const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw,
                  Name sampled = Name::Count);
    // Type-A sky geometry. The half-size depth attachment is private: all
    // accepted materials test ALWAYS and FinishSky overwrites scene depth.
    // texture must be an explicitly uploaded RGBA8 asset, never a target alias.
    Result RecordSky(const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw,
                     const Graphics::Handle& texture);
    // MovieGraphics' RGB32 picture: source-authored texel UVs into an owned
    // image, SceneColor/SceneDepth/SceneCopy attachments. TW/TH are source
    // metadata, not decoded picture dimensions. Adds no clear or present.
    Result RecordMovie(const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw,
                       const Graphics::Handle& image);
    // Source display copy uses SceneColor only, explicit private output
    // attachments, no alias/deposit or lookup through a display address.
    Result RecordFrame(const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw);
    // General unlit geometry into the named scene attachments, with an owned
    // decoded texture. TextureAssets schedules its authored mips before this.
    Result RecordMesh(const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw,
                      const Graphics::Handle& texture);

private:
    Result RecordBindings(const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw,
                          const std::vector<Graphics::Handle>& handles);
    struct Binding {
        Graphics::Handle handle;
        Graphics::Lease lease;
    };
    Graphics& queue_;
    std::array<Binding, static_cast<size_t>(Name::Count)> bindings_;
};

}  // namespace title
